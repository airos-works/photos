// The basic edits: geometry (rotate, flip, straighten, crop) and light and
// colour. An edit is a set of numbers kept with the photo; the original is
// never changed, and the same numbers render a small preview or the full
// picture.
#pragma once

#include "Image.h"

#include <string>

namespace photos {

struct EditParams {
	// Geometry, applied in this order.
	int rotation = 0;			// quarter turns clockwise, 0..3
	bool flip = false;			// mirrored left to right
	float straighten = 0;		// degrees, -45..45; positive turns the picture clockwise
	// Crop, in fractions of the turned and straightened picture.
	float cropX = 0, cropY = 0, cropW = 1, cropH = 1;

	// Light and colour: 0 leaves the picture alone, the range is -1..1
	// (exposure: stops, -2..2).
	float exposure = 0;
	float contrast = 0;
	float highlights = 0;
	float shadows = 0;
	float saturation = 0;
	float warmth = 0;
	float tint = 0;

	bool HasGeometry() const;
	bool HasAdjustments() const;
	bool IsIdentity() const { return !HasGeometry() && !HasAdjustments(); }
	bool operator==(const EditParams& other) const;
	bool operator!=(const EditParams& other) const { return !(*this == other); }

	std::string ToJson() const;
	static bool FromJson(const std::string& json, EditParams& params);
};

// The size of the edited picture made from a source of the given size.
void EditedSize(int sourceWidth, int sourceHeight, const EditParams& params,
	int& width, int& height);
// The size of the picture after turning and straightening, before the crop
// (what the crop tool shows).
void UncroppedSize(int sourceWidth, int sourceHeight, const EditParams& params,
	int& width, int& height);

// Renders the edit. maxEdge > 0 limits the longer edge of the result.
void RenderEdit(const Image& source, const EditParams& params, Image& result,
	int maxEdge = 0);
// Geometry only, and only the light and colour part (in place).
void ApplyGeometry(const Image& source, const EditParams& params, Image& result,
	int maxEdge = 0, bool withCrop = true);
void ApplyAdjustments(Image& image, const EditParams& params);

// "Enhance": light and colour values that suit the picture (geometry kept).
EditParams AutoEnhance(const Image& image, const EditParams& current);

// Turns the edited picture by quarter turns (positive: clockwise), and
// mirrors it left to right: the crop frame and the straightening go along.
void TurnEdit(EditParams& params, int quarterTurns);
void FlipEdit(EditParams& params);

// Keeps a crop rectangle inside the picture and at least a few pixels large.
void ClampCrop(EditParams& params);

}	// namespace photos
