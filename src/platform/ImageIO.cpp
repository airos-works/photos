#include "ImageIO.h"

#include <Bitmap.h>
#include <BitmapStream.h>
#include <DataIO.h>
#include <File.h>
#include <TranslationUtils.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>

#include <jpeglib.h>
#include <webp/decode.h>

#ifdef PHOTOS_HAVE_RAW
#include "photos_raw.h"
#endif

namespace photos {

namespace {

struct JpegError {
	jpeg_error_mgr manager;
	jmp_buf jump;
};


void
JpegFail(j_common_ptr info)
{
	JpegError* error = reinterpret_cast<JpegError*>(info->err);
	longjmp(error->jump, 1);
}


void
JpegQuiet(j_common_ptr, int)
{
}


// Decodes a JPEG, shrinking in the decoder by n/8 so that the longer edge
// stays at least maxEdge. The EXIF orientation is applied.
status_t
DecodeJpeg(const uint8_t* data, size_t size, Image& image, int maxEdge)
{
	if (size < 4 || data[0] != 0xff || data[1] != 0xd8)
		return B_BAD_DATA;
	Metadata metadata;
	ParseJpegMetadata(data, size, metadata);

	jpeg_decompress_struct info;
	JpegError error;
	info.err = jpeg_std_error(&error.manager);
	error.manager.error_exit = JpegFail;
	error.manager.emit_message = JpegQuiet;
	Image decoded;
	if (setjmp(error.jump) != 0) {
		jpeg_destroy_decompress(&info);
		// A picture cut short still shows what was read.
		if (!decoded.IsEmpty() && info.output_scanline > (JDIMENSION)decoded.height / 4) {
			OrientImage(decoded, metadata.orientation, image);
			return B_OK;
		}
		return B_BAD_DATA;
	}
	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, data, size);
	if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress(&info);
		return B_BAD_DATA;
	}
	if (info.jpeg_color_space == JCS_CMYK || info.jpeg_color_space == JCS_YCCK) {
		jpeg_destroy_decompress(&info);
		return B_NOT_SUPPORTED;		// the translator converts these
	}
	info.out_color_space = JCS_EXT_BGRA;
	info.scale_num = 8;
	info.scale_denom = 8;
	if (maxEdge > 0) {
		int longer = std::max(info.image_width, info.image_height);
		for (int numerator = 1; numerator <= 8; numerator++) {
			if ((int64_t)longer * numerator / 8 >= maxEdge) {
				info.scale_num = numerator;
				break;
			}
		}
	}
	info.dct_method = JDCT_IFAST;
	info.do_fancy_upsampling = maxEdge > 0 ? FALSE : TRUE;
	jpeg_start_decompress(&info);
	if (info.output_components != 4 || info.output_width == 0
		|| info.output_height == 0
		|| (uint64_t)info.output_width * info.output_height > (1ull << 29)) {
		jpeg_destroy_decompress(&info);
		return B_BAD_DATA;
	}
	decoded.Allocate(info.output_width, info.output_height);
	while (info.output_scanline < info.output_height) {
		JSAMPROW row = decoded.Row(info.output_scanline);
		jpeg_read_scanlines(&info, &row, 1);
	}
	jpeg_finish_decompress(&info);
	jpeg_destroy_decompress(&info);
	if (metadata.orientation > 1)
		OrientImage(decoded, metadata.orientation, image);
	else
		image = std::move(decoded);
	return B_OK;
}


status_t
DecodeWebP(const uint8_t* data, size_t size, Image& image)
{
	int width = 0, height = 0;
	if (!WebPGetInfo(data, size, &width, &height) || width <= 0 || height <= 0
		|| (uint64_t)width * height > (1ull << 29))
		return B_BAD_DATA;
	image.Allocate(width, height);
	if (WebPDecodeBGRAInto(data, size, image.pixels.data(), image.pixels.size(),
			width * 4) == nullptr) {
		image.Clear();
		return B_BAD_DATA;
	}
	return B_OK;
}


status_t
DecodeWithTranslator(BPositionIO& stream, Image& image)
{
	BBitmap* bitmap = BTranslationUtils::GetBitmap(&stream);
	if (bitmap == nullptr)
		return B_NO_TRANSLATOR;
	bool ok = ImageFromBitmap(bitmap, image);
	delete bitmap;
	return ok ? B_OK : B_BAD_DATA;
}


bool
IsJpeg(const uint8_t* data, size_t size)
{
	return size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
}


bool
IsWebP(const uint8_t* data, size_t size)
{
	return size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0;
}


void
Finish(Image& image, int maxEdge, uint32 flags)
{
	if (maxEdge > 0 && (flags & kDecodeExact) != 0
		&& std::max(image.width, image.height) > maxEdge) {
		Image fitted;
		FitImage(image, maxEdge, fitted);
		image = std::move(fitted);
	}
	// Opaque: pictures are shown without blending.
	if (!image.IsEmpty()) {
		uint8_t* alpha = image.pixels.data() + 3;
		size_t count = (size_t)image.width * image.height;
		// Flatten transparency onto white (PNG, WebP).
		for (size_t i = 0; i < count; i++, alpha += 4) {
			if (*alpha != 255) {
				uint32_t a = *alpha;
				alpha[-3] = (uint8_t)((alpha[-3] * a + 255 * (255 - a)) / 255);
				alpha[-2] = (uint8_t)((alpha[-2] * a + 255 * (255 - a)) / 255);
				alpha[-1] = (uint8_t)((alpha[-1] * a + 255 * (255 - a)) / 255);
				*alpha = 255;
			}
		}
	}
}


#ifdef PHOTOS_HAVE_RAW
void
TakeRawImage(photos_raw_image& raw, Image& image)
{
	image.Allocate((int)raw.width, (int)raw.height);
	for (uint32_t y = 0; y < raw.height; y++) {
		memcpy(image.Row((int)y), raw.pixels + (size_t)y * raw.stride,
			(size_t)raw.width * 4);
	}
	photos_raw_free_image(&raw);
}
#endif


status_t
DecodeRaw(const std::string& path, Image& image, int maxEdge, uint32 flags,
	std::string* error)
{
#ifdef PHOTOS_HAVE_RAW
	char message[256] = "";
	if ((flags & kDecodeRawPreview) != 0) {
		uint8_t* jpeg = nullptr;
		size_t jpegSize = 0;
		if (photos_raw_preview_file(path.c_str(), &jpeg, &jpegSize, message,
				sizeof(message)) == 0 && jpeg != nullptr) {
			// A preview much smaller than asked for is of no use.
			Metadata preview;
			bool usable = ParseJpegMetadata(jpeg, jpegSize, preview)
				&& (maxEdge <= 0 ? std::max(preview.width, preview.height) >= 1600
					: std::max(preview.width, preview.height) * 3 >= maxEdge * 2);
			status_t status = usable ? DecodeJpeg(jpeg, jpegSize, image, maxEdge)
				: B_ERROR;
			// Previews without an orientation of their own lie as the
			// sensor does.
			if (status == B_OK && preview.orientation <= 1) {
				photos_raw_info info;
				memset(&info, 0, sizeof(info));
				if (photos_raw_info_file(path.c_str(), &info, message,
						sizeof(message)) == 0 && info.orientation > 1) {
					bool previewPortrait = image.height > image.width;
					bool developedPortrait = info.height > info.width;
					if (previewPortrait != developedPortrait
						|| info.orientation == 3) {
						Image turned;
						OrientImage(image, info.orientation, turned);
						image = std::move(turned);
					}
				}
			}
			photos_raw_free(jpeg, jpegSize);
			if (status == B_OK)
				return B_OK;
		}
	}
	photos_raw_image raw;
	memset(&raw, 0, sizeof(raw));
	int result = photos_raw_develop_file(path.c_str(),
		maxEdge > 0 ? (uint32_t)maxEdge : 0, &raw, message, sizeof(message));
	if (result != 0 || raw.pixels == nullptr) {
		if (error != nullptr)
			*error = message[0] != '\0' ? message : "cannot develop this RAW file";
		// The system's RAW translator knows other cameras.
		BFile file(path.c_str(), B_READ_ONLY);
		if (file.InitCheck() == B_OK && DecodeWithTranslator(file, image) == B_OK)
			return B_OK;
		return B_NOT_SUPPORTED;
	}
	TakeRawImage(raw, image);
	return B_OK;
#else
	(void)maxEdge;
	(void)flags;
	BFile file(path.c_str(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return file.InitCheck();
	status_t status = DecodeWithTranslator(file, image);
	if (status != B_OK && error != nullptr)
		*error = "RAW files are not supported in this build";
	return status;
#endif
}

}	// namespace


bool
HaveRawEngine()
{
#ifdef PHOTOS_HAVE_RAW
	return true;
#else
	return false;
#endif
}


std::string
RawEngineVersion()
{
#ifdef PHOTOS_HAVE_RAW
	const char* version = photos_raw_version();
	return version != nullptr ? version : "";
#else
	return std::string();
#endif
}


status_t
DecodeMemory(const void* data, size_t size, Image& image, int maxEdge, uint32 flags)
{
	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	status_t status = B_NO_TRANSLATOR;
	if (IsJpeg(bytes, size))
		status = DecodeJpeg(bytes, size, image, maxEdge);
	else if (IsWebP(bytes, size))
		status = DecodeWebP(bytes, size, image);
	if (status != B_OK) {
		BMemoryIO stream(data, size);
		status = DecodeWithTranslator(stream, image);
	}
	if (status == B_OK)
		Finish(image, maxEdge, flags);
	return status;
}


status_t
DecodeFile(const std::string& path, Image& image, int maxEdge, uint32 flags,
	std::string* error)
{
	image.Clear();
	MediaKind kind = KindOfName(path);
	status_t status;
	if (kind == kKindRaw) {
		status = DecodeRaw(path, image, maxEdge, flags, error);
		if (status == B_OK)
			Finish(image, maxEdge, flags);
		return status;
	}
#ifdef PHOTOS_HAVE_HEIF
	if (kind == kKindHeic) {
		char message[256] = "";
		photos_raw_image raw;
		memset(&raw, 0, sizeof(raw));
		if (photos_heif_decode_file(path.c_str(), maxEdge > 0 ? (uint32_t)maxEdge : 0,
				&raw, message, sizeof(message)) == 0 && raw.pixels != nullptr) {
			TakeRawImage(raw, image);
			Finish(image, maxEdge, flags);
			return B_OK;
		}
		if (error != nullptr)
			*error = message;
	}
#endif
	std::string data;
	if (!ReadFile(path, data) || data.empty()) {
		if (error != nullptr)
			*error = "cannot read the file";
		return B_ENTRY_NOT_FOUND;
	}
	status = DecodeMemory(data.data(), data.size(), image, maxEdge, flags);
	if (status != B_OK && error != nullptr && error->empty())
		*error = "no translator reads this kind of picture";
	return status;
}


bool
ReadFileMetadata(const std::string& path, MediaKind kind, Metadata& metadata)
{
#ifdef PHOTOS_HAVE_RAW
	if (kind == kKindRaw) {
		photos_raw_info info;
		memset(&info, 0, sizeof(info));
		char message[256] = "";
		if (photos_raw_info_file(path.c_str(), &info, message, sizeof(message)) == 0) {
			// The developed size is the size as shown.
			metadata.width = (int)info.width;
			metadata.height = (int)info.height;
			metadata.orientation = 1;
			metadata.taken = info.taken;
			metadata.make = info.make;
			metadata.model = info.model;
			metadata.lens = info.lens;
			metadata.iso = info.iso;
			metadata.exposureTime = info.exposure_time;
			metadata.fNumber = info.f_number;
			metadata.focalLength = info.focal_length;
			if (info.has_gps && std::isfinite(info.latitude)
				&& std::isfinite(info.longitude)) {
				metadata.hasGps = true;
				metadata.latitude = info.latitude;
				metadata.longitude = info.longitude;
			}
			return true;
		}
	}
#endif
#ifdef PHOTOS_HAVE_HEIF
	if (kind == kKindHeic) {
		photos_raw_info info;
		memset(&info, 0, sizeof(info));
		char message[256] = "";
		if (photos_heif_info_file(path.c_str(), &info, message, sizeof(message)) == 0) {
			metadata.width = (int)info.width;
			metadata.height = (int)info.height;
			metadata.orientation = 1;
			metadata.taken = info.taken;
			metadata.make = info.make;
			metadata.model = info.model;
			metadata.lens = info.lens;
			metadata.iso = info.iso;
			metadata.exposureTime = info.exposure_time;
			metadata.fNumber = info.f_number;
			metadata.focalLength = info.focal_length;
			if (info.has_gps) {
				metadata.hasGps = true;
				metadata.latitude = info.latitude;
				metadata.longitude = info.longitude;
			}
			return true;
		}
	}
#endif
	(void)kind;
	// A TIFF-like file may carry EXIF directly.
	std::string head;
	if (ReadFile(path, head, 1 << 20) && head.size() > 8
		&& (memcmp(head.data(), "II*\0", 4) == 0 || memcmp(head.data(), "MM\0*", 4) == 0)) {
		ParseExif((const uint8_t*)head.data(), head.size(), metadata);
	}
	if (metadata.width == 0) {
		// Ask the translators for the size.
		BFile file(path.c_str(), B_READ_ONLY);
		BTranslatorRoster* roster = BTranslatorRoster::Default();
		translator_info info;
		if (file.InitCheck() == B_OK && roster != nullptr
			&& roster->Identify(&file, nullptr, &info) == B_OK) {
			BMallocIO header;
			file.Seek(0, SEEK_SET);
			BMessage settings;
			settings.AddBool(B_TRANSLATOR_EXT_HEADER_ONLY, true);
			if (roster->Translate(&file, &info, &settings, &header,
					B_TRANSLATOR_BITMAP) == B_OK
				&& header.BufferLength() >= sizeof(TranslatorBitmap)) {
				TranslatorBitmap bitmap;
				memcpy(&bitmap, header.Buffer(), sizeof(bitmap));
				BRect bounds;
				bounds.left = B_BENDIAN_TO_HOST_FLOAT(bitmap.bounds.left);
				bounds.top = B_BENDIAN_TO_HOST_FLOAT(bitmap.bounds.top);
				bounds.right = B_BENDIAN_TO_HOST_FLOAT(bitmap.bounds.right);
				bounds.bottom = B_BENDIAN_TO_HOST_FLOAT(bitmap.bounds.bottom);
				if (bounds.IsValid() && bounds.Width() < 100000
					&& bounds.Height() < 100000) {
					metadata.width = bounds.IntegerWidth() + 1;
					metadata.height = bounds.IntegerHeight() + 1;
				}
			}
		}
	}
	return metadata.width > 0 || metadata.taken != 0;
}


bool
EncodeJpeg(const Image& image, int quality, std::vector<uint8_t>& data)
{
	if (image.IsEmpty())
		return false;
	jpeg_compress_struct info;
	JpegError error;
	info.err = jpeg_std_error(&error.manager);
	error.manager.error_exit = JpegFail;
	unsigned char* buffer = nullptr;
	unsigned long bufferSize = 0;
	if (setjmp(error.jump) != 0) {
		jpeg_destroy_compress(&info);
		free(buffer);
		return false;
	}
	jpeg_create_compress(&info);
	jpeg_mem_dest(&info, &buffer, &bufferSize);
	info.image_width = image.width;
	info.image_height = image.height;
	info.input_components = 4;
	info.in_color_space = JCS_EXT_BGRA;
	jpeg_set_defaults(&info);
	jpeg_set_quality(&info, quality, TRUE);
	info.optimize_coding = TRUE;
	if (quality >= 90) {
		// No chroma subsampling for pictures that will be edited again.
		info.comp_info[0].h_samp_factor = 1;
		info.comp_info[0].v_samp_factor = 1;
	}
	jpeg_start_compress(&info, TRUE);
	while (info.next_scanline < info.image_height) {
		JSAMPROW row = const_cast<JSAMPROW>(image.Row(info.next_scanline));
		jpeg_write_scanlines(&info, &row, 1);
	}
	jpeg_finish_compress(&info);
	jpeg_destroy_compress(&info);
	data.assign(buffer, buffer + bufferSize);
	free(buffer);
	return true;
}


bool
WriteJpeg(const Image& image, int quality, const std::string& path)
{
	std::vector<uint8_t> data;
	if (!EncodeJpeg(image, quality, data))
		return false;
	MakeDirs(DirName(path));
	return WriteFile(path, data.data(), data.size());
}


bool
WritePng(const Image& image, const std::string& path)
{
	BBitmap* bitmap = MakeBitmap(image);
	if (bitmap == nullptr)
		return false;
	MakeDirs(DirName(path));
	std::string temporary = path + ".part";
	bool ok = false;
	{
		BBitmapStream stream(bitmap);	// owns the bitmap
		BFile file(temporary.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		BTranslatorRoster* roster = BTranslatorRoster::Default();
		ok = file.InitCheck() == B_OK && roster != nullptr
			&& roster->Translate(&stream, nullptr, nullptr, &file, B_PNG_FORMAT) == B_OK;
	}
	if (ok)
		ok = rename(temporary.c_str(), path.c_str()) == 0;
	if (!ok)
		remove(temporary.c_str());
	return ok;
}


BBitmap*
MakeBitmap(const Image& image)
{
	if (image.IsEmpty())
		return nullptr;
	BBitmap* bitmap = new(std::nothrow) BBitmap(
		BRect(0, 0, image.width - 1, image.height - 1), B_RGB32);
	if (bitmap == nullptr || bitmap->InitCheck() != B_OK) {
		delete bitmap;
		return nullptr;
	}
	uint8* bits = (uint8*)bitmap->Bits();
	int32 rowBytes = bitmap->BytesPerRow();
	for (int y = 0; y < image.height; y++)
		memcpy(bits + (size_t)y * rowBytes, image.Row(y), (size_t)image.width * 4);
	return bitmap;
}


bool
ImageFromBitmap(const BBitmap* bitmap, Image& image)
{
	if (bitmap == nullptr || bitmap->InitCheck() != B_OK)
		return false;
	color_space space = bitmap->ColorSpace();
	int width = bitmap->Bounds().IntegerWidth() + 1;
	int height = bitmap->Bounds().IntegerHeight() + 1;
	const uint8* bits = (const uint8*)bitmap->Bits();
	int32 rowBytes = bitmap->BytesPerRow();
	if (space == B_RGB32 || space == B_RGBA32) {
		image.Allocate(width, height);
		for (int y = 0; y < height; y++) {
			memcpy(image.Row(y), bits + (size_t)y * rowBytes, (size_t)width * 4);
			if (space == B_RGB32) {
				uint8_t* row = image.Row(y);
				for (int x = 0; x < width; x++)
					row[x * 4 + 3] = 255;
			}
		}
		return true;
	}
	if (space == B_RGB24) {
		image.Allocate(width, height);
		for (int y = 0; y < height; y++) {
			const uint8* in = bits + (size_t)y * rowBytes;
			uint8_t* out = image.Row(y);
			for (int x = 0; x < width; x++) {
				out[x * 4] = in[x * 3];
				out[x * 4 + 1] = in[x * 3 + 1];
				out[x * 4 + 2] = in[x * 3 + 2];
				out[x * 4 + 3] = 255;
			}
		}
		return true;
	}
	// Anything else: let the Interface Kit convert.
	BBitmap converted(bitmap->Bounds(), B_RGB32);
	if (converted.InitCheck() != B_OK
		|| converted.ImportBits(bitmap) != B_OK)
		return false;
	return ImageFromBitmap(&converted, image);
}

}	// namespace photos
