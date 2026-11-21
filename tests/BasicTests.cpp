#include "Test.h"

#include "core/Edit.h"
#include "core/Exif.h"
#include "core/Image.h"
#include "core/Util.h"

#include <cstring>

using namespace photos;

TEST(Sha1KnownValues)
{
	// SHA-1("abc") = a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d
	CHECK_EQ(Sha1OfData("abc", 3), HexToBase64("a9993e364706816aba3e25717850c26c9cd0d89d"));
	CHECK_EQ(Sha1OfData("", 0), HexToBase64("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
	std::string million(1000000, 'a');
	CHECK_EQ(Sha1OfData(million.data(), million.size()),
		HexToBase64("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
	CHECK_EQ(Base64Decode("aGVsbG8="), std::string("hello"));
}

TEST(CivilTimeRoundTrip)
{
	CivilTime time;
	time.year = 2024; time.month = 2; time.day = 29; time.hour = 23; time.minute = 59; time.second = 58;
	int64_t seconds = CivilToSeconds(time);
	CHECK_EQ(seconds, (int64_t)1709251198);
	CivilTime back = SecondsToCivil(seconds);
	CHECK_EQ(back.year, 2024);
	CHECK_EQ(back.month, 2);
	CHECK_EQ(back.day, 29);
	CHECK_EQ(back.hour, 23);
	CHECK_EQ(back.second, 58);
	CHECK_EQ(DayOfWeek(seconds), 4);	// a Thursday
	CivilTime early = SecondsToCivil(-86400 * 366);
	CHECK_EQ(early.year, 1968);
	CHECK_EQ(early.month, 12);
	CHECK_EQ(early.day, 31);
}

TEST(ParseDates)
{
	CivilTime time;
	CHECK(ParseDateTime("2022:09:22 14:01:01", time));
	CHECK_EQ(time.year, 2022);
	CHECK_EQ(time.hour, 14);
	CHECK(ParseDateTime("2022-09-22T14:01:01.000Z", time));
	CHECK_EQ(time.minute, 1);
	CHECK_EQ(time.second, 1);
	CHECK(ParseDateTime("20240309T140533.5+1000", time));
	CHECK_EQ(time.month, 3);
	CHECK_EQ(time.second, 33);
	CHECK(ParseDateTime("2024-03-09", time));
	CHECK_EQ(time.hour, 0);
	CHECK(!ParseDateTime("", time));
	CHECK(!ParseDateTime("0000:00:00 00:00:00", time));
	CHECK(!ParseDateTime("garbage", time));
	int zone = 0;
	CHECK(ParseZoneOffset("+10:00", zone));
	CHECK_EQ(zone, 36000);
	CHECK(ParseZoneOffset("2024-03-09T14:05:33-0330", zone));
	CHECK_EQ(zone, -12600);
	CHECK(ParseZoneOffset("2024-03-09T14:05:33Z", zone));
	CHECK_EQ(zone, 0);
	CHECK(!ParseZoneOffset("2024-03-09T14:05:33", zone));
}

TEST(NamesAndKinds)
{
	CHECK_EQ(Extension("/a/b/IMG_1.JPG"), std::string("jpg"));
	CHECK_EQ(Extension("/a.b/file"), std::string(""));
	CHECK_EQ(StripExtension("/a.b/file.tar.gz"), std::string("/a.b/file.tar"));
	CHECK_EQ(StripExtension(".hidden"), std::string(".hidden"));
	CHECK_EQ((int)KindOfName("DSCF1725.RAF"), (int)kKindRaw);
	CHECK_EQ((int)KindOfName("a.heic"), (int)kKindHeic);
	CHECK_EQ((int)KindOfName("a.MOV"), (int)kKindVideo);
	CHECK_EQ((int)KindOfName("notes.txt"), (int)kKindUnknown);
	CHECK(IsPictureName("x.webp"));
	CHECK(!IsPictureName("x.mp4"));
	CHECK_EQ(JoinPath("/a/", "/b"), std::string("/a/b"));
}

// A small EXIF block: big endian, IFD0 with Make, Orientation, ExifIFD;
// ExifIFD with DateTimeOriginal, OffsetTimeOriginal, ISO, FNumber.
static std::string
MakeExif()
{
	std::string t;
	auto u16 = [&](unsigned v) { t.push_back((char)(v >> 8)); t.push_back((char)v); };
	auto u32 = [&](unsigned v) { u16(v >> 16); u16(v & 0xffff); };
	t += "MM"; u16(42); u32(8);
	// IFD0 at 8: 3 entries -> 2 + 36 + 4 = 42 bytes, ends at 50
	u16(3);
	u16(0x010f); u16(2); u32(4); t += "ACME";		// 4 bytes, inline
	u16(0x0112); u16(3); u32(1); u16(6); u16(0);
	u16(0x8769); u16(4); u32(1); u32(50);
	u32(0);
	// Exif IFD at 50: 4 entries -> 2 + 48 + 4 = 54, data from 104
	u16(4);
	u16(0x9003); u16(2); u32(20); u32(104);
	u16(0x9011); u16(2); u32(7); u32(124);
	u16(0x8827); u16(3); u32(1); u16(400); u16(0);
	u16(0x829d); u16(5); u32(1); u32(131);
	u32(0);
	t += std::string("2023:07:04 09:08:07", 19); t.push_back('\0');		// 104..123
	t += std::string("+02:00", 6); t.push_back('\0');						// 124..130
	u32(28); u32(10);														// 131: 2.8
	return t;
}

TEST(ExifBasics)
{
	std::string exif = MakeExif();
	Metadata metadata;
	CHECK(ParseExif((const uint8_t*)exif.data(), exif.size(), metadata));
	CHECK_EQ(metadata.make, std::string("ACME"));
	CHECK_EQ(metadata.orientation, 6);
	CivilTime time = SecondsToCivil(metadata.taken);
	CHECK_EQ(time.year, 2023);
	CHECK_EQ(time.month, 7);
	CHECK_EQ(time.hour, 9);
	CHECK(metadata.hasZone);
	CHECK_EQ(metadata.zoneOffset, 7200);
	CHECK_EQ((int)metadata.iso, 400);
	CHECK(metadata.fNumber > 2.79 && metadata.fNumber < 2.81);

	// In a JPEG: SOI, APP1, SOF0 6x4, EOI.
	std::string jpeg("\xff\xd8", 2);
	std::string app1 = std::string("Exif\0\0", 6) + exif;
	jpeg += "\xff\xe1";
	jpeg.push_back((char)((app1.size() + 2) >> 8));
	jpeg.push_back((char)(app1.size() + 2));
	jpeg += app1;
	jpeg += std::string("\xff\xc0\x00\x11\x08\x00\x04\x00\x06\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01", 19);
	jpeg += "\xff\xd9";
	Metadata fromJpeg;
	CHECK(ParseJpegMetadata((const uint8_t*)jpeg.data(), jpeg.size(), fromJpeg));
	CHECK_EQ(fromJpeg.width, 6);
	CHECK_EQ(fromJpeg.height, 4);
	CHECK_EQ(fromJpeg.DisplayWidth(), 4);
	CHECK_EQ(fromJpeg.orientation, 6);

	// Every truncation and a few corruptions must be survived.
	for (size_t cut = 0; cut < exif.size(); cut++) {
		Metadata ignored;
		ParseExif((const uint8_t*)exif.data(), cut, ignored);
	}
	for (size_t i = 0; i < exif.size(); i++) {
		std::string damaged = exif;
		damaged[i] = (char)0xff;
		Metadata ignored;
		ParseExif((const uint8_t*)damaged.data(), damaged.size(), ignored);
	}
	for (size_t cut = 0; cut < jpeg.size(); cut++) {
		Metadata ignored;
		ParseJpegMetadata((const uint8_t*)jpeg.data(), cut, ignored);
	}
}

static Image
Gradient(int width, int height)
{
	Image image;
	image.Allocate(width, height);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint8_t* p = image.Row(y) + x * 4;
			p[0] = (uint8_t)(x * 255 / std::max(1, width - 1));
			p[1] = (uint8_t)(y * 255 / std::max(1, height - 1));
			p[2] = 128;
			p[3] = 255;
		}
	}
	return image;
}

TEST(ScaleAndOrient)
{
	Image source = Gradient(64, 48);
	Image half;
	ScaleImage(source, 32, 24, half);
	CHECK_EQ(half.width, 32);
	CHECK_EQ(half.height, 24);
	// The average of the left two columns' blue.
	CHECK(abs(half.Row(0)[0] - (source.Row(0)[0] + source.Row(0)[4]) / 2) <= 1);
	CHECK_EQ((int)half.Row(5)[2], 128);
	CHECK_EQ((int)half.Row(5)[3], 255);
	Image odd;
	ScaleImage(source, 7, 5, odd);
	CHECK_EQ(odd.width, 7);
	CHECK(odd.Row(4)[6 * 4] > 200);		// right edge is bright blue
	Image large;
	ScaleImage(source, 100, 100, large);
	CHECK_EQ(large.height, 100);
	Image mixed;
	ScaleImage(source, 100, 10, mixed);
	CHECK_EQ(mixed.width, 100);

	Image turned;
	OrientImage(source, 6, turned);		// 90 degrees clockwise
	CHECK_EQ(turned.width, 48);
	CHECK_EQ(turned.height, 64);
	// The source's top-left ends up top-right.
	CHECK_EQ(memcmp(turned.Row(0) + 47 * 4, source.Row(0), 4), 0);
	// Its bottom-left ends up top-left.
	CHECK_EQ(memcmp(turned.Row(0), source.Row(47), 4), 0);
	Image upside;
	OrientImage(source, 3, upside);
	CHECK_EQ(memcmp(upside.Row(47) + 63 * 4, source.Row(0), 4), 0);

	int w, h;
	FitSize(4000, 3000, 512, w, h);
	CHECK_EQ(w, 512);
	CHECK_EQ(h, 384);
	FitSize(300, 200, 512, w, h);
	CHECK_EQ(w, 300);
	Image cover;
	CoverImage(source, 20, 20, cover);
	CHECK_EQ(cover.width, 20);
	// A centred square: the left edge is not the source's left edge.
	CHECK(cover.Row(10)[0] > 20);
}

TEST(EditGeometry)
{
	Image source = Gradient(80, 60);
	EditParams params;
	CHECK(params.IsIdentity());
	Image same;
	RenderEdit(source, params, same);
	CHECK_EQ(same.pixels == source.pixels, true);

	params.rotation = 1;
	int w, h;
	EditedSize(80, 60, params, w, h);
	CHECK_EQ(w, 60);
	CHECK_EQ(h, 80);
	Image turned;
	RenderEdit(source, params, turned);
	CHECK_EQ(turned.width, 60);
	// Clockwise: the source's bottom-left (dark blue, bright green) is top-left.
	CHECK(turned.Row(0)[0] < 10);
	CHECK(turned.Row(0)[1] > 245);
	// And it matches the EXIF turn.
	Image exifTurned;
	OrientImage(source, 6, exifTurned);
	CHECK_EQ(memcmp(turned.Row(10) + 40, exifTurned.Row(10) + 40, 4), 0);

	params = EditParams();
	params.flip = true;
	Image flipped;
	RenderEdit(source, params, flipped);
	CHECK(flipped.Row(0)[0] > 245);		// bright blue now on the left

	params = EditParams();
	params.cropX = 0.5f; params.cropY = 0.5f; params.cropW = 0.5f; params.cropH = 0.5f;
	Image cropped;
	RenderEdit(source, params, cropped);
	CHECK_EQ(cropped.width, 40);
	CHECK_EQ(cropped.height, 30);
	CHECK(abs(cropped.Row(0)[0] - source.Row(30)[40 * 4]) <= 3);

	params = EditParams();
	params.straighten = 10;
	EditedSize(80, 60, params, w, h);
	CHECK(w < 80 && h < 60);
	CHECK(abs(w * 60 - h * 80) < 80);		// same aspect ratio
	Image straight;
	RenderEdit(source, params, straight);
	CHECK_EQ(straight.width, w);

	Image small;
	RenderEdit(Gradient(800, 600), params, small, 100);
	CHECK_EQ(std::max(small.width, small.height), 100);

	EditParams bad;
	bad.cropX = 5; bad.cropW = -1; bad.cropH = 1e9f;
	ClampCrop(bad);
	CHECK(bad.cropX >= 0 && bad.cropX + bad.cropW <= 1.0001f);
}

TEST(EditAdjustments)
{
	Image source = Gradient(64, 64);
	auto mean = [](const Image& image) {
		double sum = 0;
		for (size_t i = 0; i < image.pixels.size(); i += 4)
			sum += image.pixels[i] + image.pixels[i + 1] + image.pixels[i + 2];
		return sum / (image.pixels.size() / 4 * 3);
	};
	EditParams params;
	params.exposure = 1;
	Image brighter = source;
	ApplyAdjustments(brighter, params);
	CHECK(mean(brighter) > mean(source) + 20);
	params.exposure = -1;
	Image darker = source;
	ApplyAdjustments(darker, params);
	CHECK(mean(darker) < mean(source) - 20);

	params = EditParams();
	params.saturation = -1;
	Image grey = source;
	ApplyAdjustments(grey, params);
	for (int i = 0; i < 64 * 64; i += 97) {
		const uint8_t* p = grey.pixels.data() + i * 4;
		CHECK(abs(p[0] - p[1]) <= 1 && abs(p[1] - p[2]) <= 1);
	}

	params = EditParams();
	params.warmth = 1;
	Image warm = source;
	ApplyAdjustments(warm, params);
	const uint8_t* before = source.Row(32) + 32 * 4;
	const uint8_t* after = warm.Row(32) + 32 * 4;
	CHECK(after[2] > before[2]);	// more red
	CHECK(after[0] < before[0]);	// less blue
	CHECK_EQ((int)after[3], 255);

	params = EditParams();
	params.contrast = 1;
	Image contrasty = source;
	ApplyAdjustments(contrasty, params);
	// Dark gets darker, bright brighter (green runs top to bottom).
	CHECK(contrasty.Row(8)[1] <= source.Row(8)[1]);
	CHECK(contrasty.Row(60)[1] >= source.Row(60)[1]);

	// JSON round trip.
	params.rotation = 3; params.flip = true; params.straighten = -3.5f;
	params.cropX = 0.1f; params.cropW = 0.8f; params.tint = -0.25f;
	EditParams back;
	CHECK(EditParams::FromJson(params.ToJson(), back));
	CHECK(back == params);
	CHECK(!EditParams::FromJson("not json", back));
	CHECK(!EditParams::FromJson("[1,2]", back));
	CHECK(EditParams::FromJson("{\"exposure\": 99, \"crop\": [\"a\", 0, 1, 1]}", back));
	CHECK(back.exposure <= 2.0f);

	// A dark, flat picture gets lifted.
	Image dull;
	dull.Allocate(40, 40);
	for (size_t i = 0; i < dull.pixels.size(); i += 4) {
		uint8_t v = (uint8_t)(40 + (i / 4) % 60);
		dull.pixels[i] = dull.pixels[i + 1] = dull.pixels[i + 2] = v;
		dull.pixels[i + 3] = 255;
	}
	EditParams enhanced = AutoEnhance(dull, EditParams());
	CHECK(enhanced.exposure > 0.2f);
	CHECK(enhanced.contrast > 0);
}

TEST(ThumbHash)
{
	// From the Immich server's own data (a portrait picture).
	std::string hash = Base64Decode("1eYNJQRgeEqIR4pWiIiHd5+m+XmZ");
	Image image;
	CHECK(DecodeThumbHash((const uint8_t*)hash.data(), hash.size(), image));
	CHECK(image.width > 0 && image.height > image.width);
	CHECK(image.height <= 32);
	CHECK_EQ((int)image.Row(0)[3], 255);
	for (size_t cut = 0; cut < hash.size(); cut++) {
		Image ignored;
		DecodeThumbHash((const uint8_t*)hash.data(), cut, ignored);
	}
}
