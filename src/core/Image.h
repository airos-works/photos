// A picture in memory: 8 bits per channel, B G R A (the layout of a Haiku
// B_RGB32 bitmap), rows without padding.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace photos {

struct Image {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;

	bool IsEmpty() const { return width <= 0 || height <= 0; }
	int Stride() const { return width * 4; }
	uint8_t* Row(int y) { return pixels.data() + (size_t)y * width * 4; }
	const uint8_t* Row(int y) const { return pixels.data() + (size_t)y * width * 4; }
	void Allocate(int w, int h);
	void Clear();
};

// Runs body(firstRow, endRow) over [0, rows) on several threads.
void ParallelRows(int rows, const std::function<void(int, int)>& body,
	int minRowsPerThread = 64);

// Resamples to exactly width x height: area averaging when shrinking,
// bilinear when enlarging.
void ScaleImage(const Image& source, int width, int height, Image& result);
// Shrinks so that the longer edge is at most maxEdge (never enlarges).
void FitImage(const Image& source, int maxEdge, Image& result);
// The size that fits (width, height) into maxEdge on the longer side.
void FitSize(int width, int height, int maxEdge, int& outWidth, int& outHeight);
// Applies an EXIF orientation (1..8) to stored pixels.
void OrientImage(const Image& source, int orientation, Image& result);
// A centred crop with the aspect ratio of width:height, scaled to that size.
void CoverImage(const Image& source, int width, int height, Image& result);
// A region given in fractions of the picture, scaled to width x height.
void CropImage(const Image& source, float x, float y, float w, float h,
	int width, int height, Image& result);

// ThumbHash (Immich's placeholders): the blurred picture it stands for, at
// most 32 pixels on the longer side. False for damaged data.
bool DecodeThumbHash(const uint8_t* hash, size_t size, Image& result);

}	// namespace photos
