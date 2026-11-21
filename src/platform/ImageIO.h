// Reading and writing picture files: JPEG (libjpeg-turbo, with its fast
// reduced-size decoding), WebP, camera RAW and HEIF (the RAW library, built
// from LightCraft), everything else through the Translation Kit.
#pragma once

#include "core/Exif.h"
#include "core/Image.h"
#include "core/Util.h"

#include <SupportDefs.h>

#include <string>
#include <vector>

class BBitmap;

namespace photos {

enum {
	// RAW files: decode the camera's embedded preview instead of developing
	// the sensor data (fast; falls back to developing when there is none).
	kDecodeRawPreview = 1,
};

// Decodes a file, turned upright. With maxEdge > 0 the result is at least
// that large on its longer side when the file is (decoders shrink by whole
// factors), unless kDecodeExact asks to scale it to fit.
enum { kDecodeExact = 2 };
status_t DecodeFile(const std::string& path, Image& image, int maxEdge = 0,
	uint32 flags = 0, std::string* error = nullptr);
status_t DecodeMemory(const void* data, size_t size, Image& image, int maxEdge = 0,
	uint32 flags = 0);

// What the file says about itself, for kinds the library cannot read by
// itself (RAW, HEIF) and the size of anything a translator knows.
bool ReadFileMetadata(const std::string& path, MediaKind kind, Metadata& metadata);

bool EncodeJpeg(const Image& image, int quality, std::vector<uint8_t>& data);
bool WriteJpeg(const Image& image, int quality, const std::string& path);
// PNG through the Translation Kit (lossless hand-over to other programs).
bool WritePng(const Image& image, const std::string& path);

// A B_RGB32 bitmap with the image's pixels; the caller owns it.
BBitmap* MakeBitmap(const Image& image);
bool ImageFromBitmap(const BBitmap* bitmap, Image& image);

// Whether camera RAW files are developed by the LightCraft engine in this
// build (otherwise their embedded previews are shown).
bool HaveRawEngine();
std::string RawEngineVersion();

}	// namespace photos
