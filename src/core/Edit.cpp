#include "Edit.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace photos {

namespace {

const float kEpsilon = 1e-4f;


float
Clamp(float value, float low, float high)
{
	return std::max(low, std::min(high, value));
}


float
SrgbToLinear(float value)
{
	return value <= 0.04045f ? value / 12.92f
		: powf((value + 0.055f) / 1.055f, 2.4f);
}


float
LinearToSrgb(float value)
{
	if (value <= 0)
		return 0;
	if (value >= 1)
		return 1;
	return value <= 0.0031308f ? value * 12.92f
		: 1.055f * powf(value, 1 / 2.4f) - 0.055f;
}


// The scale that makes a w x h rectangle, turned by `degrees`, fit inside
// the unturned one with the same aspect ratio.
double
InscribedScale(double w, double h, double degrees)
{
	double radians = fabs(degrees) * M_PI / 180;
	double c = cos(radians), s = sin(radians);
	return std::min(w / (w * c + h * s), h / (w * s + h * c));
}

}	// namespace


bool
EditParams::HasGeometry() const
{
	return rotation % 4 != 0 || flip || fabsf(straighten) > kEpsilon
		|| cropX > kEpsilon || cropY > kEpsilon || cropW < 1 - kEpsilon
		|| cropH < 1 - kEpsilon;
}


bool
EditParams::HasAdjustments() const
{
	return fabsf(exposure) > kEpsilon || fabsf(contrast) > kEpsilon
		|| fabsf(highlights) > kEpsilon || fabsf(shadows) > kEpsilon
		|| fabsf(saturation) > kEpsilon || fabsf(warmth) > kEpsilon
		|| fabsf(tint) > kEpsilon;
}


bool
EditParams::operator==(const EditParams& other) const
{
	auto same = [](float a, float b) { return fabsf(a - b) <= kEpsilon; };
	return (rotation - other.rotation) % 4 == 0 && flip == other.flip
		&& same(straighten, other.straighten) && same(cropX, other.cropX)
		&& same(cropY, other.cropY) && same(cropW, other.cropW)
		&& same(cropH, other.cropH) && same(exposure, other.exposure)
		&& same(contrast, other.contrast) && same(highlights, other.highlights)
		&& same(shadows, other.shadows) && same(saturation, other.saturation)
		&& same(warmth, other.warmth) && same(tint, other.tint);
}


std::string
EditParams::ToJson() const
{
	nlohmann::json json;
	json["rotation"] = ((rotation % 4) + 4) % 4;
	json["flip"] = flip;
	json["straighten"] = straighten;
	json["crop"] = {cropX, cropY, cropW, cropH};
	json["exposure"] = exposure;
	json["contrast"] = contrast;
	json["highlights"] = highlights;
	json["shadows"] = shadows;
	json["saturation"] = saturation;
	json["warmth"] = warmth;
	json["tint"] = tint;
	return json.dump();
}


bool
EditParams::FromJson(const std::string& text, EditParams& params)
{
	nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
	if (!json.is_object())
		return false;
	EditParams read;
	auto number = [&](const char* key, float fallback) {
		auto found = json.find(key);
		return found != json.end() && found->is_number()
			? found->get<float>() : fallback;
	};
	read.rotation = (((int)number("rotation", 0) % 4) + 4) % 4;
	auto flip = json.find("flip");
	read.flip = flip != json.end() && flip->is_boolean() && flip->get<bool>();
	read.straighten = Clamp(number("straighten", 0), -45, 45);
	auto crop = json.find("crop");
	if (crop != json.end() && crop->is_array() && crop->size() == 4) {
		bool numbers = true;
		for (auto& value : *crop)
			numbers = numbers && value.is_number();
		if (numbers) {
			read.cropX = (*crop)[0].get<float>();
			read.cropY = (*crop)[1].get<float>();
			read.cropW = (*crop)[2].get<float>();
			read.cropH = (*crop)[3].get<float>();
		}
	}
	read.exposure = Clamp(number("exposure", 0), -2, 2);
	read.contrast = Clamp(number("contrast", 0), -1, 1);
	read.highlights = Clamp(number("highlights", 0), -1, 1);
	read.shadows = Clamp(number("shadows", 0), -1, 1);
	read.saturation = Clamp(number("saturation", 0), -1, 1);
	read.warmth = Clamp(number("warmth", 0), -1, 1);
	read.tint = Clamp(number("tint", 0), -1, 1);
	ClampCrop(read);
	params = read;
	return true;
}


void
ClampCrop(EditParams& params)
{
	auto finite = [](float value, float fallback) {
		return std::isfinite(value) ? value : fallback;
	};
	params.cropW = Clamp(finite(params.cropW, 1), 0.02f, 1);
	params.cropH = Clamp(finite(params.cropH, 1), 0.02f, 1);
	params.cropX = Clamp(finite(params.cropX, 0), 0, 1 - params.cropW);
	params.cropY = Clamp(finite(params.cropY, 0), 0, 1 - params.cropH);
}


void
UncroppedSize(int sourceWidth, int sourceHeight, const EditParams& params,
	int& width, int& height)
{
	bool turned = (params.rotation % 2 + 2) % 2 == 1;
	double w = turned ? sourceHeight : sourceWidth;
	double h = turned ? sourceWidth : sourceHeight;
	double scale = InscribedScale(w, h, params.straighten);
	width = std::max(1, (int)floor(w * scale + 0.5));
	height = std::max(1, (int)floor(h * scale + 0.5));
}


void
EditedSize(int sourceWidth, int sourceHeight, const EditParams& params,
	int& width, int& height)
{
	int w, h;
	UncroppedSize(sourceWidth, sourceHeight, params, w, h);
	width = std::max(1, (int)lround(w * params.cropW));
	height = std::max(1, (int)lround(h * params.cropH));
}


void
ApplyGeometry(const Image& source, const EditParams& params, Image& result,
	int maxEdge, bool withCrop)
{
	if (source.IsEmpty()) {
		result.Clear();
		return;
	}
	EditParams p = params;
	if (!withCrop) {
		p.cropX = p.cropY = 0;
		p.cropW = p.cropH = 1;
	}
	ClampCrop(p);
	if (!p.HasGeometry()) {
		if (maxEdge > 0)
			FitImage(source, maxEdge, result);
		else
			result = source;
		return;
	}

	// Much more source than result: average it down first so that the
	// bilinear sampling below does not skip pixels.
	int fullWidth, fullHeight;
	EditedSize(source.width, source.height, p, fullWidth, fullHeight);
	int width, height;
	FitSize(fullWidth, fullHeight, maxEdge, width, height);
	const Image* input = &source;
	Image reduced;
	double shrink = (double)fullWidth / width;
	if (shrink > 1.5) {
		int reducedWidth = std::max(1, (int)lround(source.width / shrink));
		int reducedHeight = std::max(1, (int)lround(source.height / shrink));
		ScaleImage(source, reducedWidth, reducedHeight, reduced);
		input = &reduced;
	}

	int quarter = ((p.rotation % 4) + 4) % 4;
	bool turned = quarter % 2 == 1;
	double sw = input->width, sh = input->height;
	// The turned picture's size, and the straightened one inside it.
	double tw = turned ? sh : sw;
	double th = turned ? sw : sh;
	double scale = InscribedScale(tw, th, p.straighten);
	double iw = tw * scale, ih = th * scale;
	double radians = p.straighten * M_PI / 180;
	double cosA = cos(radians), sinA = sin(radians);

	result.Allocate(width, height);
	double stepX = iw * p.cropW / width;
	double stepY = ih * p.cropH / height;
	ParallelRows(height, [&](int first, int end) {
		for (int y = first; y < end; y++) {
			uint8_t* out = result.Row(y);
			// Position in the straightened picture, from its centre.
			double v = (p.cropY * ih + (y + 0.5) * stepY) - ih / 2;
			for (int x = 0; x < width; x++) {
				double u = (p.cropX * iw + (x + 0.5) * stepX) - iw / 2;
				// Undo the straightening: the picture was turned clockwise
				// by the angle, so look up turned back.
				double tx = u * cosA + v * sinA + tw / 2;
				double ty = -u * sinA + v * cosA + th / 2;
				if (p.flip)
					tx = tw - tx;
				// Undo the quarter turns.
				double sx, sy;
				switch (quarter) {
					case 1: sx = ty; sy = sh - tx; break;
					case 2: sx = sw - tx; sy = sh - ty; break;
					case 3: sx = sw - ty; sy = tx; break;
					default: sx = tx; sy = ty; break;
				}
				sx -= 0.5;
				sy -= 0.5;
				sx = std::max(0.0, std::min(sw - 1.0, sx));
				sy = std::max(0.0, std::min(sh - 1.0, sy));
				int x0 = (int)sx, y0 = (int)sy;
				int x1 = std::min(x0 + 1, input->width - 1);
				int y1 = std::min(y0 + 1, input->height - 1);
				int fx = (int)((sx - x0) * 256), fy = (int)((sy - y0) * 256);
				const uint8_t* row0 = input->Row(y0);
				const uint8_t* row1 = input->Row(y1);
				for (int c = 0; c < 4; c++) {
					int top = row0[x0 * 4 + c] * (256 - fx) + row0[x1 * 4 + c] * fx;
					int bottom = row1[x0 * 4 + c] * (256 - fx) + row1[x1 * 4 + c] * fx;
					out[x * 4 + c] = (uint8_t)((top * (256 - fy) + bottom * fy + 32768) >> 16);
				}
			}
		}
	}, 32);
}


void
ApplyAdjustments(Image& image, const EditParams& params)
{
	if (image.IsEmpty() || !params.HasAdjustments())
		return;

	// White balance and exposure work on linear light; one table per
	// channel takes a stored value to the adjusted one, back in the sRGB
	// curve (0..1, may exceed 1 before the roll-off below).
	float gain = powf(2.0f, params.exposure);
	float warm = params.warmth, tint = params.tint;
	float gains[3] = {
		gain * (1.0f - 0.22f * warm + 0.06f * tint),	// blue
		gain * (1.0f - 0.16f * tint),					// green
		gain * (1.0f + 0.22f * warm + 0.06f * tint)		// red
	};
	// Keep the overall brightness when only the colour changes.
	float balance = (gains[0] * 0.0722f + gains[1] * 0.7152f + gains[2] * 0.2126f) / gain;
	float channel[3][256];
	for (int c = 0; c < 3; c++) {
		for (int i = 0; i < 256; i++) {
			float linear = SrgbToLinear(i / 255.0f) * gains[c] / balance;
			// A soft shoulder instead of clipping what the exposure pushes
			// over white.
			if (linear > 0.8f)
				linear = 0.8f + 0.2f * (1.0f - expf(-(linear - 0.8f) / 0.2f));
			channel[c][i] = LinearToSrgb(linear);
		}
	}

	// The tone curve works on brightness: shadows and highlights lift or
	// lower their end of the range, contrast pivots around the middle.
	const int kToneSize = 1024;
	float tone[kToneSize + 1];
	float contrast = params.contrast;
	for (int i = 0; i <= kToneSize; i++) {
		float l = (float)i / kToneSize;
		float value = l;
		float shadowWeight = (1 - l) * (1 - l) * (1 - l) * l * 4.0f;
		float highlightWeight = l * l * l * (1 - l) * 4.0f;
		value += params.shadows * 0.9f * shadowWeight;
		value += params.highlights * 0.9f * highlightWeight;
		if (fabsf(contrast) > kEpsilon) {
			// An S-curve through (0,0), (0.5,0.5), (1,1); flattened for
			// negative values.
			float t = value;
			float s = t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
			if (contrast > 0)
				value = t + (s - t) * contrast * 0.9f;
			else
				value = t + (0.5f - t) * (-contrast) * 0.6f;
		}
		tone[i] = Clamp(value, 0, 1);
	}
	float saturation = params.saturation >= 0 ? 1 + params.saturation
		: 1 + params.saturation;	// -1: grey, +1: twice

	ParallelRows(image.height, [&](int first, int end) {
		for (int y = first; y < end; y++) {
			uint8_t* pixel = image.Row(y);
			for (int x = 0; x < image.width; x++, pixel += 4) {
				float b = channel[0][pixel[0]];
				float g = channel[1][pixel[1]];
				float r = channel[2][pixel[2]];
				float luma = 0.0722f * b + 0.7152f * g + 0.2126f * r;
				float position = Clamp(luma, 0, 1) * kToneSize;
				int index = (int)position;
				float toned = index >= kToneSize ? tone[kToneSize]
					: tone[index] + (tone[index + 1] - tone[index]) * (position - index);
				if (luma > 1e-4f) {
					float ratio = toned / luma;
					b *= ratio;
					g *= ratio;
					r *= ratio;
				} else
					b = g = r = toned;
				if (saturation != 1.0f) {
					b = toned + (b - toned) * saturation;
					g = toned + (g - toned) * saturation;
					r = toned + (r - toned) * saturation;
				}
				pixel[0] = (uint8_t)(Clamp(b, 0, 1) * 255.0f + 0.5f);
				pixel[1] = (uint8_t)(Clamp(g, 0, 1) * 255.0f + 0.5f);
				pixel[2] = (uint8_t)(Clamp(r, 0, 1) * 255.0f + 0.5f);
			}
		}
	});
}


void
RenderEdit(const Image& source, const EditParams& params, Image& result,
	int maxEdge)
{
	ApplyGeometry(source, params, result, maxEdge);
	ApplyAdjustments(result, params);
}


EditParams
AutoEnhance(const Image& image, const EditParams& current)
{
	EditParams params = current;
	params.exposure = params.contrast = params.highlights = params.shadows = 0;
	params.saturation = params.warmth = params.tint = 0;
	if (image.IsEmpty())
		return params;

	// Brightness histogram of a sample of the picture (sRGB curve).
	uint32_t histogram[256] = {};
	double sumSaturation = 0;
	uint64_t count = 0;
	int step = std::max(1, std::max(image.width, image.height) / 400);
	for (int y = 0; y < image.height; y += step) {
		const uint8_t* pixel = image.Row(y);
		for (int x = 0; x < image.width; x += step) {
			const uint8_t* p = pixel + (size_t)x * 4;
			int luma = (p[0] * 18 + p[1] * 183 + p[2] * 54) >> 8;
			histogram[std::min(luma, 255)]++;
			int high = std::max(p[0], std::max(p[1], p[2]));
			int low = std::min(p[0], std::min(p[1], p[2]));
			sumSaturation += high > 0 ? (double)(high - low) / high : 0;
			count++;
		}
	}
	if (count == 0)
		return params;
	auto percentile = [&](double fraction) {
		uint64_t target = (uint64_t)(count * fraction);
		uint64_t seen = 0;
		for (int i = 0; i < 256; i++) {
			seen += histogram[i];
			if (seen > target)
				return i;
		}
		return 255;
	};
	float low = percentile(0.01) / 255.0f;
	float middle = percentile(0.5) / 255.0f;
	float high = percentile(0.99) / 255.0f;

	// Bring the brightest tones near white, but not if that takes more than
	// a stop: a dark picture may be meant to be dark.
	float highLinear = std::max(SrgbToLinear(high), 0.02f);
	float exposure = log2f(SrgbToLinear(0.96f) / highLinear);
	// And the middle towards the middle.
	float middleAfter = LinearToSrgb(SrgbToLinear(middle) * powf(2, exposure));
	if (middleAfter < 0.36f)
		params.shadows = Clamp((0.42f - middleAfter) * 2.2f, 0, 0.6f);
	else if (middleAfter > 0.62f)
		params.highlights = -Clamp((middleAfter - 0.55f) * 1.6f, 0, 0.5f);
	params.exposure = Clamp(exposure, -0.5f, 1.0f);
	// A flat picture gets contrast; a vivid one is left alone.
	float range = high - low;
	if (range < 0.75f)
		params.contrast = Clamp((0.8f - range) * 0.9f, 0, 0.45f);
	else
		params.contrast = 0.08f;
	float meanSaturation = (float)(sumSaturation / count);
	if (meanSaturation < 0.35f && meanSaturation > 0.04f)
		params.saturation = Clamp((0.38f - meanSaturation) * 0.8f, 0, 0.22f);
	return params;
}

}	// namespace photos
