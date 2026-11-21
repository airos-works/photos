#include "Exif.h"

#include "Util.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace photos {

namespace {

// A bounds-checked view of a TIFF structure.
class Tiff {
public:
	Tiff(const uint8_t* data, size_t size)
		:
		fData(data),
		fSize(size),
		fBig(false)
	{
	}

	bool Init(uint32_t& firstIfd)
	{
		if (fSize < 8)
			return false;
		if (fData[0] == 'M' && fData[1] == 'M')
			fBig = true;
		else if (fData[0] == 'I' && fData[1] == 'I')
			fBig = false;
		else
			return false;
		if (U16(2) != 42)
			return false;
		firstIfd = U32(4);
		return true;
	}

	uint16_t U16(size_t offset) const
	{
		if (offset + 2 > fSize)
			return 0;
		return fBig ? (uint16_t)(fData[offset] << 8 | fData[offset + 1])
			: (uint16_t)(fData[offset + 1] << 8 | fData[offset]);
	}

	uint32_t U32(size_t offset) const
	{
		if (offset + 4 > fSize)
			return 0;
		return fBig
			? (uint32_t)fData[offset] << 24 | (uint32_t)fData[offset + 1] << 16
				| (uint32_t)fData[offset + 2] << 8 | fData[offset + 3]
			: (uint32_t)fData[offset + 3] << 24 | (uint32_t)fData[offset + 2] << 16
				| (uint32_t)fData[offset + 1] << 8 | fData[offset];
	}

	struct Entry {
		uint16_t tag = 0;
		uint16_t type = 0;
		uint32_t count = 0;
		size_t value = 0;	// offset of the value bytes
	};

	// Calls `visit` for every entry of the directory; returns the next one.
	template<typename Visit>
	uint32_t Directory(uint32_t offset, Visit visit) const
	{
		if (offset == 0 || (size_t)offset + 2 > fSize)
			return 0;
		uint16_t count = U16(offset);
		size_t at = (size_t)offset + 2;
		for (uint16_t i = 0; i < count; i++, at += 12) {
			if (at + 12 > fSize)
				return 0;
			Entry entry;
			entry.tag = U16(at);
			entry.type = U16(at + 2);
			entry.count = U32(at + 4);
			static const uint8_t kSizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
			size_t unit = entry.type < sizeof(kSizes) ? kSizes[entry.type] : 0;
			if (unit == 0 || entry.count > fSize)
				continue;
			size_t bytes = unit * entry.count;
			entry.value = bytes <= 4 ? at + 8 : (size_t)U32(at + 8);
			if (entry.value + bytes > fSize)
				continue;
			visit(entry);
		}
		return at + 4 <= fSize ? U32(at) : 0;
	}

	std::string Text(const Entry& entry) const
	{
		if (entry.type != 2)
			return std::string();
		std::string text((const char*)fData + entry.value, entry.count);
		size_t end = text.find('\0');
		if (end != std::string::npos)
			text.resize(end);
		return Trim(text);
	}

	double Number(const Entry& entry, uint32_t index = 0) const
	{
		if (index >= entry.count)
			return 0;
		switch (entry.type) {
			case 1:
			case 7:
				return fData[entry.value + index];
			case 3:
				return U16(entry.value + index * 2);
			case 4:
				return U32(entry.value + index * 4);
			case 9:
				return (int32_t)U32(entry.value + index * 4);
			case 5:
			{
				uint32_t denominator = U32(entry.value + index * 8 + 4);
				return denominator == 0 ? 0
					: (double)U32(entry.value + index * 8) / denominator;
			}
			case 10:
			{
				int32_t denominator = (int32_t)U32(entry.value + index * 8 + 4);
				return denominator == 0 ? 0
					: (double)(int32_t)U32(entry.value + index * 8) / denominator;
			}
		}
		return 0;
	}

private:
	const uint8_t* fData;
	size_t fSize;
	bool fBig;
};

}	// namespace


bool
ParseExif(const uint8_t* data, size_t size, Metadata& metadata)
{
	Tiff tiff(data, size);
	uint32_t ifd0;
	if (!tiff.Init(ifd0))
		return false;

	uint32_t exifIfd = 0, gpsIfd = 0;
	std::string dateTime, original, digitized, offsetOriginal, offset;
	int tiffWidth = 0, tiffHeight = 0;
	tiff.Directory(ifd0, [&](const Tiff::Entry& entry) {
		switch (entry.tag) {
			// A TIFF picture's own size (an EXIF block in a JPEG has none).
			case 0x0100: tiffWidth = (int)tiff.Number(entry); break;
			case 0x0101: tiffHeight = (int)tiff.Number(entry); break;
			case 0x010f: metadata.make = tiff.Text(entry); break;
			case 0x0110: metadata.model = tiff.Text(entry); break;
			case 0x0112:
			{
				int value = (int)tiff.Number(entry);
				if (value >= 1 && value <= 8)
					metadata.orientation = value;
				break;
			}
			case 0x0132: dateTime = tiff.Text(entry); break;
			case 0x8769: exifIfd = (uint32_t)tiff.Number(entry); break;
			case 0x8825: gpsIfd = (uint32_t)tiff.Number(entry); break;
		}
	});
	int pixelWidth = 0, pixelHeight = 0;
	tiff.Directory(exifIfd, [&](const Tiff::Entry& entry) {
		switch (entry.tag) {
			case 0x829a: metadata.exposureTime = tiff.Number(entry); break;
			case 0x829d: metadata.fNumber = tiff.Number(entry); break;
			case 0x8827: metadata.iso = tiff.Number(entry); break;
			case 0x9003: original = tiff.Text(entry); break;
			case 0x9004: digitized = tiff.Text(entry); break;
			case 0x9010: offset = tiff.Text(entry); break;
			case 0x9011: offsetOriginal = tiff.Text(entry); break;
			case 0x920a: metadata.focalLength = tiff.Number(entry); break;
			case 0xa002: pixelWidth = (int)tiff.Number(entry); break;
			case 0xa003: pixelHeight = (int)tiff.Number(entry); break;
			case 0xa434: metadata.lens = tiff.Text(entry); break;
		}
	});
	if (metadata.width == 0 && tiffWidth > 0 && tiffHeight > 0
		&& tiffWidth < 100000 && tiffHeight < 100000) {
		metadata.width = tiffWidth;
		metadata.height = tiffHeight;
	}
	if (metadata.width == 0 && pixelWidth > 0 && pixelHeight > 0) {
		metadata.width = pixelWidth;
		metadata.height = pixelHeight;
	}

	const std::string& when = !original.empty() ? original
		: !digitized.empty() ? digitized : dateTime;
	CivilTime civil;
	if (ParseDateTime(when, civil)) {
		metadata.taken = CivilToSeconds(civil);
		int zone;
		const std::string& zoneText = !offsetOriginal.empty()
			? offsetOriginal : offset;
		if (!zoneText.empty() && ParseZoneOffset(zoneText, zone)) {
			metadata.hasZone = true;
			metadata.zoneOffset = zone;
		}
	}

	if (gpsIfd != 0) {
		std::string latitudeRef, longitudeRef;
		double latitude = NAN, longitude = NAN;
		auto degrees = [&](const Tiff::Entry& entry) {
			if (entry.count < 3 || (entry.type != 5 && entry.type != 10))
				return (double)NAN;
			return tiff.Number(entry, 0) + tiff.Number(entry, 1) / 60
				+ tiff.Number(entry, 2) / 3600;
		};
		tiff.Directory(gpsIfd, [&](const Tiff::Entry& entry) {
			switch (entry.tag) {
				case 1: latitudeRef = tiff.Text(entry); break;
				case 2: latitude = degrees(entry); break;
				case 3: longitudeRef = tiff.Text(entry); break;
				case 4: longitude = degrees(entry); break;
			}
		});
		if (std::isfinite(latitude) && std::isfinite(longitude)
			&& fabs(latitude) <= 90 && fabs(longitude) <= 180
			&& (latitude != 0 || longitude != 0)) {
			metadata.hasGps = true;
			metadata.latitude = latitudeRef == "S" ? -latitude : latitude;
			metadata.longitude = longitudeRef == "W" ? -longitude : longitude;
		}
	}
	return true;
}


bool
ParseJpegMetadata(const uint8_t* data, size_t size, Metadata& metadata)
{
	if (size < 4 || data[0] != 0xff || data[1] != 0xd8)
		return false;
	size_t at = 2;
	bool haveSize = false, haveExif = false;
	while (at + 4 <= size) {
		if (data[at] != 0xff) {
			at++;
			continue;
		}
		uint8_t marker = data[at + 1];
		if (marker == 0xff) {
			at++;
			continue;
		}
		if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) {
			at += 2;
			continue;
		}
		if (marker == 0xd9 || marker == 0xda)
			break;
		size_t length = (size_t)data[at + 2] << 8 | data[at + 3];
		if (length < 2 || at + 2 + length > size)
			break;
		const uint8_t* body = data + at + 4;
		size_t bodySize = length - 2;
		if (marker == 0xe1 && !haveExif && bodySize > 6
			&& memcmp(body, "Exif\0\0", 6) == 0) {
			haveExif = ParseExif(body + 6, bodySize - 6, metadata);
		} else if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4
			&& marker != 0xc8 && marker != 0xcc && bodySize >= 5) {
			metadata.height = body[1] << 8 | body[2];
			metadata.width = body[3] << 8 | body[4];
			haveSize = true;
		}
		at += 2 + length;
		if (haveSize && haveExif)
			break;
	}
	return haveSize;
}


bool
ReadJpegMetadata(const std::string& path, Metadata& metadata)
{
	// The frame header follows the application segments: EXIF, ICC, MPF and
	// maker previews are well below this on real files.
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	std::vector<uint8_t> head(1 << 20);
	size_t got = fread(head.data(), 1, head.size(), file);
	bool ok = ParseJpegMetadata(head.data(), got, metadata);
	if (!ok && got == head.size()) {
		// A very large header: read on.
		head.resize(16 << 20);
		got += fread(head.data() + got, 1, head.size() - got, file);
		metadata = Metadata();
		ok = ParseJpegMetadata(head.data(), got, metadata);
	}
	fclose(file);
	return ok;
}


bool
ReadImageSize(const std::string& path, int& width, int& height)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	uint8_t head[64];
	size_t got = fread(head, 1, sizeof(head), file);
	fclose(file);
	auto be32 = [&](size_t at) {
		return (int)((uint32_t)head[at] << 24 | (uint32_t)head[at + 1] << 16
			| (uint32_t)head[at + 2] << 8 | head[at + 3]);
	};
	auto le16 = [&](size_t at) { return (int)(head[at] | head[at + 1] << 8); };
	auto le24 = [&](size_t at) {
		return (int)(head[at] | head[at + 1] << 8 | head[at + 2] << 16);
	};
	if (got >= 24 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0) {
		width = be32(16);
		height = be32(20);
		return width > 0 && height > 0;
	}
	if (got >= 10 && (memcmp(head, "GIF87a", 6) == 0 || memcmp(head, "GIF89a", 6) == 0)) {
		width = le16(6);
		height = le16(8);
		return true;
	}
	if (got >= 26 && head[0] == 'B' && head[1] == 'M') {
		width = le16(18) | le16(20) << 16;
		height = abs(le16(22) | le16(24) << 16);
		return width > 0;
	}
	if (got >= 30 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WEBP", 4) == 0) {
		if (memcmp(head + 12, "VP8X", 4) == 0) {
			width = le24(24) + 1;
			height = le24(27) + 1;
			return true;
		}
		if (memcmp(head + 12, "VP8 ", 4) == 0) {
			width = le16(26) & 0x3fff;
			height = le16(28) & 0x3fff;
			return true;
		}
		if (memcmp(head + 12, "VP8L", 4) == 0) {
			uint32_t bits = (uint32_t)head[21] | (uint32_t)head[22] << 8
				| (uint32_t)head[23] << 16 | (uint32_t)head[24] << 24;
			width = (int)(bits & 0x3fff) + 1;
			height = (int)(bits >> 14 & 0x3fff) + 1;
			return true;
		}
	}
	return false;
}


bool
LooksLikePicture(const std::string& path, int kind)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	uint8_t head[32] = {};
	size_t got = fread(head, 1, sizeof(head), file);
	fclose(file);
	auto begins = [&](const char* text, size_t length, size_t at = 0) {
		return got >= at + length && memcmp(head + at, text, length) == 0;
	};
	bool jpeg = got >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff;
	bool png = begins("\x89PNG\r\n\x1a\n", 8);
	bool isoMedia = begins("ftyp", 4, 4);
	switch (kind) {
		case kKindJpeg:
			return jpeg;
		case kKindPng:
			return png;
		case kKindHeic:
			return isoMedia;
		case kKindRaw:
			return got >= 16;
		case kKindOther:
			return jpeg || png || isoMedia || begins("GIF8", 4) || begins("BM", 2)
				|| begins("II*\0", 4) || begins("MM\0*", 4) || begins("8BPS", 4)
				|| (begins("RIFF", 4) && begins("WEBP", 4, 8))
				|| begins("\0\0\0\x0cjP  ", 8) || begins("\xff\x0a", 2)
				|| begins("\0\0\0\x0cJXL ", 8);
	}
	return false;
}


std::string
FormatExposure(double seconds)
{
	char text[32];
	if (seconds <= 0)
		return std::string();
	if (seconds >= 1)
		snprintf(text, sizeof(text), seconds == floor(seconds) ? "%.0f s" : "%.1f s", seconds);
	else
		snprintf(text, sizeof(text), "1/%.0f s", 1 / seconds);
	return text;
}


std::string
FormatAperture(double fNumber)
{
	char text[32];
	if (fNumber <= 0)
		return std::string();
	snprintf(text, sizeof(text), fNumber == floor(fNumber) ? "f/%.0f" : "f/%.1f", fNumber);
	return text;
}

}	// namespace photos
