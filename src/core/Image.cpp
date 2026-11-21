#include "Image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace photos {

void
Image::Allocate(int w, int h)
{
	width = std::max(w, 0);
	height = std::max(h, 0);
	pixels.assign((size_t)width * height * 4, 0);
}


void
Image::Clear()
{
	width = height = 0;
	pixels.clear();
	pixels.shrink_to_fit();
}


void
ParallelRows(int rows, const std::function<void(int, int)>& body,
	int minRowsPerThread)
{
	if (rows <= 0)
		return;
	int threads = (int)std::thread::hardware_concurrency();
	threads = std::max(1, std::min(threads, 16));
	threads = std::min(threads, std::max(1, rows / std::max(minRowsPerThread, 1)));
	if (threads <= 1) {
		body(0, rows);
		return;
	}
	std::vector<std::thread> workers;
	int per = (rows + threads - 1) / threads;
	for (int i = 1; i < threads; i++) {
		int first = i * per;
		int end = std::min(rows, first + per);
		if (first >= end)
			break;
		workers.emplace_back([=, &body]() { body(first, end); });
	}
	body(0, std::min(rows, per));
	for (auto& worker : workers)
		worker.join();
}


void
FitSize(int width, int height, int maxEdge, int& outWidth, int& outHeight)
{
	outWidth = width;
	outHeight = height;
	if (maxEdge <= 0 || width <= 0 || height <= 0
		|| std::max(width, height) <= maxEdge)
		return;
	if (width >= height) {
		outWidth = maxEdge;
		outHeight = std::max(1, (int)((int64_t)height * maxEdge / width));
	} else {
		outHeight = maxEdge;
		outWidth = std::max(1, (int)((int64_t)width * maxEdge / height));
	}
}


namespace {

// One axis of an area-averaging resample: for each destination index the
// source span [start, end) and the weights of its first and last samples
// (in 1/256).
struct Span {
	int start;
	int end;
	int firstWeight;
	int lastWeight;
};


void
MakeSpans(int source, int target, std::vector<Span>& spans)
{
	spans.resize(target);
	double scale = (double)source / target;
	for (int i = 0; i < target; i++) {
		double from = i * scale;
		double to = (i + 1) * scale;
		int start = (int)floor(from);
		int end = std::min(source, (int)ceil(to));
		if (end <= start)
			end = start + 1;
		Span& span = spans[i];
		span.start = start;
		span.end = end;
		span.firstWeight = (int)lround((1.0 - (from - start)) * 256);
		span.lastWeight = (int)lround((1.0 - (end - to)) * 256);
		if (end - start == 1)
			span.firstWeight = span.lastWeight = 256;
		span.firstWeight = std::max(1, std::min(256, span.firstWeight));
		span.lastWeight = std::max(1, std::min(256, span.lastWeight));
	}
}


void
ShrinkImage(const Image& source, int width, int height, Image& result)
{
	result.Allocate(width, height);
	std::vector<Span> columns, rows;
	MakeSpans(source.width, width, columns);
	MakeSpans(source.height, height, rows);
	ParallelRows(height, [&](int first, int end) {
		// Sums of one destination row: horizontal pass per source row,
		// accumulated vertically.
		std::vector<uint64_t> sums((size_t)width * 4);
		std::vector<uint32_t> line((size_t)width * 4);
		std::vector<uint32_t> weights(width);
		for (int x = 0; x < width; x++) {
			const Span& span = columns[x];
			uint32_t weight = 0;
			for (int sx = span.start; sx < span.end; sx++) {
				weight += sx == span.start ? span.firstWeight
					: sx == span.end - 1 ? span.lastWeight : 256;
			}
			weights[x] = weight;
		}
		for (int y = first; y < end; y++) {
			const Span& rowSpan = rows[y];
			std::fill(sums.begin(), sums.end(), 0);
			uint64_t rowWeight = 0;
			for (int sy = rowSpan.start; sy < rowSpan.end; sy++) {
				const uint8_t* in = source.Row(sy);
				for (int x = 0; x < width; x++) {
					const Span& span = columns[x];
					uint32_t b = 0, g = 0, r = 0, a = 0;
					const uint8_t* pixel = in + (size_t)span.start * 4;
					for (int sx = span.start; sx < span.end; sx++, pixel += 4) {
						uint32_t weight = sx == span.start ? span.firstWeight
							: sx == span.end - 1 ? span.lastWeight : 256;
						b += pixel[0] * weight;
						g += pixel[1] * weight;
						r += pixel[2] * weight;
						a += pixel[3] * weight;
					}
					line[x * 4] = b;
					line[x * 4 + 1] = g;
					line[x * 4 + 2] = r;
					line[x * 4 + 3] = a;
				}
				uint32_t weight = sy == rowSpan.start ? rowSpan.firstWeight
					: sy == rowSpan.end - 1 ? rowSpan.lastWeight : 256;
				rowWeight += weight;
				for (size_t i = 0; i < sums.size(); i++)
					sums[i] += (uint64_t)line[i] * weight;
			}
			uint8_t* out = result.Row(y);
			for (int x = 0; x < width; x++) {
				uint64_t total = (uint64_t)weights[x] * rowWeight;
				for (int c = 0; c < 4; c++)
					out[x * 4 + c] = (uint8_t)((sums[x * 4 + c] + total / 2) / total);
			}
		}
	}, 16);
}


void
EnlargeImage(const Image& source, int width, int height, Image& result)
{
	result.Allocate(width, height);
	double scaleX = (double)source.width / width;
	double scaleY = (double)source.height / height;
	ParallelRows(height, [&](int first, int end) {
		for (int y = first; y < end; y++) {
			double sy = std::max(0.0, (y + 0.5) * scaleY - 0.5);
			int y0 = std::min((int)sy, source.height - 1);
			int y1 = std::min(y0 + 1, source.height - 1);
			int fy = (int)((sy - y0) * 256);
			const uint8_t* row0 = source.Row(y0);
			const uint8_t* row1 = source.Row(y1);
			uint8_t* out = result.Row(y);
			for (int x = 0; x < width; x++) {
				double sx = std::max(0.0, (x + 0.5) * scaleX - 0.5);
				int x0 = std::min((int)sx, source.width - 1);
				int x1 = std::min(x0 + 1, source.width - 1);
				int fx = (int)((sx - x0) * 256);
				for (int c = 0; c < 4; c++) {
					int top = row0[x0 * 4 + c] * (256 - fx) + row0[x1 * 4 + c] * fx;
					int bottom = row1[x0 * 4 + c] * (256 - fx) + row1[x1 * 4 + c] * fx;
					out[x * 4 + c] = (uint8_t)((top * (256 - fy) + bottom * fy + 32768) >> 16);
				}
			}
		}
	}, 32);
}

}	// namespace


void
ScaleImage(const Image& source, int width, int height, Image& result)
{
	width = std::max(width, 1);
	height = std::max(height, 1);
	if (source.IsEmpty()) {
		result.Allocate(width, height);
		return;
	}
	if (width == source.width && height == source.height) {
		result = source;
		return;
	}
	if (width <= source.width && height <= source.height)
		ShrinkImage(source, width, height, result);
	else if (width >= source.width && height >= source.height)
		EnlargeImage(source, width, height, result);
	else {
		// Shrinks one way and grows the other: two steps.
		Image step;
		ShrinkImage(source, std::min(width, source.width),
			std::min(height, source.height), step);
		EnlargeImage(step, width, height, result);
	}
}


void
FitImage(const Image& source, int maxEdge, Image& result)
{
	int width, height;
	FitSize(source.width, source.height, maxEdge, width, height);
	ScaleImage(source, width, height, result);
}


void
OrientImage(const Image& source, int orientation, Image& result)
{
	if (orientation < 2 || orientation > 8) {
		result = source;
		return;
	}
	bool turned = orientation >= 5;
	int width = turned ? source.height : source.width;
	int height = turned ? source.width : source.height;
	result.Allocate(width, height);
	int sw = source.width, sh = source.height;
	ParallelRows(height, [&](int first, int end) {
		for (int y = first; y < end; y++) {
			uint32_t* out = (uint32_t*)result.Row(y);
			for (int x = 0; x < width; x++) {
				int sx, sy;
				switch (orientation) {
					case 2: sx = sw - 1 - x; sy = y; break;
					case 3: sx = sw - 1 - x; sy = sh - 1 - y; break;
					case 4: sx = x; sy = sh - 1 - y; break;
					case 5: sx = y; sy = x; break;
					case 6: sx = y; sy = sh - 1 - x; break;
					case 7: sx = sw - 1 - y; sy = sh - 1 - x; break;
					default: sx = sw - 1 - y; sy = x; break;	// 8
				}
				out[x] = ((const uint32_t*)source.Row(sy))[sx];
			}
		}
	});
}


void
CropImage(const Image& source, float x, float y, float w, float h,
	int width, int height, Image& result)
{
	int left = std::max(0, std::min(source.width - 1, (int)lround(x * source.width)));
	int top = std::max(0, std::min(source.height - 1, (int)lround(y * source.height)));
	int cropWidth = std::max(1, std::min(source.width - left, (int)lround(w * source.width)));
	int cropHeight = std::max(1, std::min(source.height - top, (int)lround(h * source.height)));
	Image crop;
	crop.Allocate(cropWidth, cropHeight);
	for (int row = 0; row < cropHeight; row++) {
		memcpy(crop.Row(row), source.Row(top + row) + (size_t)left * 4,
			(size_t)cropWidth * 4);
	}
	ScaleImage(crop, width, height, result);
}


void
CoverImage(const Image& source, int width, int height, Image& result)
{
	if (source.IsEmpty()) {
		result.Allocate(width, height);
		return;
	}
	double aspect = (double)width / height;
	double sourceAspect = (double)source.width / source.height;
	float w = 1, h = 1;
	if (sourceAspect > aspect)
		w = (float)(aspect / sourceAspect);
	else
		h = (float)(sourceAspect / aspect);
	CropImage(source, (1 - w) / 2, (1 - h) / 2, w, h, width, height, result);
}


// ThumbHash, after Evan Wallace's reference implementation (MIT).
bool
DecodeThumbHash(const uint8_t* hash, size_t size, Image& result)
{
	if (size < 5)
		return false;
	uint32_t header24 = hash[0] | (uint32_t)hash[1] << 8 | (uint32_t)hash[2] << 16;
	uint32_t header16 = hash[3] | (uint32_t)hash[4] << 8;
	float lDc = (header24 & 63) / 63.0f;
	float pDc = ((header24 >> 6) & 63) / 31.5f - 1.0f;
	float qDc = ((header24 >> 12) & 63) / 31.5f - 1.0f;
	float lScale = ((header24 >> 18) & 31) / 31.0f;
	bool hasAlpha = (header24 >> 23) != 0;
	float pScale = ((header16 >> 3) & 63) / 63.0f;
	float qScale = ((header16 >> 9) & 63) / 63.0f;
	bool isLandscape = (header16 >> 15) != 0;
	int lx = std::max(3, isLandscape ? (hasAlpha ? 5 : 7) : (int)(header16 & 7));
	int ly = std::max(3, isLandscape ? (int)(header16 & 7) : (hasAlpha ? 5 : 7));
	float aDc = 1.0f, aScale = 0;
	if (hasAlpha) {
		if (size < 6)
			return false;
		aDc = (hash[5] & 15) / 15.0f;
		aScale = (hash[5] >> 4) / 15.0f;
	}

	size_t acStart = hasAlpha ? 6 : 5;
	size_t acIndex = 0;
	bool damaged = false;
	auto decodeChannel = [&](int nx, int ny, float scale, std::vector<float>& ac) {
		for (int cy = 0; cy < ny; cy++) {
			for (int cx = cy ? 0 : 1; cx * ny < nx * (ny - cy); cx++) {
				size_t byte = acStart + (acIndex >> 1);
				if (byte >= size) {
					damaged = true;
					ac.push_back(0);
				} else {
					ac.push_back((((hash[byte] >> ((acIndex & 1) << 2)) & 15)
						/ 7.5f - 1.0f) * scale);
				}
				acIndex++;
			}
		}
	};
	std::vector<float> lAc, pAc, qAc, aAc;
	decodeChannel(lx, ly, lScale, lAc);
	decodeChannel(3, 3, pScale * 1.25f, pAc);
	decodeChannel(3, 3, qScale * 1.25f, qAc);
	if (hasAlpha)
		decodeChannel(5, 5, aScale, aAc);
	if (damaged)
		return false;

	float ratio = (float)lx / ly;
	int width = (int)lround(ratio > 1 ? 32.0f : 32.0f * ratio);
	int height = (int)lround(ratio > 1 ? 32.0f / ratio : 32.0f);
	result.Allocate(width, height);
	std::vector<float> fx(std::max(lx, hasAlpha ? 5 : 3));
	std::vector<float> fy(std::max(ly, hasAlpha ? 5 : 3));
	for (int y = 0; y < height; y++) {
		uint8_t* out = result.Row(y);
		for (int x = 0; x < width; x++) {
			float l = lDc, p = pDc, q = qDc, a = aDc;
			for (size_t cx = 0; cx < fx.size(); cx++)
				fx[cx] = cosf((float)M_PI / width * (x + 0.5f) * cx);
			for (size_t cy = 0; cy < fy.size(); cy++)
				fy[cy] = cosf((float)M_PI / height * (y + 0.5f) * cy);
			for (int cy = 0, j = 0; cy < ly; cy++) {
				float fy2 = fy[cy] * 2;
				for (int cx = cy ? 0 : 1; cx * ly < lx * (ly - cy); cx++, j++)
					l += lAc[j] * fx[cx] * fy2;
			}
			for (int cy = 0, j = 0; cy < 3; cy++) {
				float fy2 = fy[cy] * 2;
				for (int cx = cy ? 0 : 1; cx < 3 - cy; cx++, j++) {
					float f = fx[cx] * fy2;
					p += pAc[j] * f;
					q += qAc[j] * f;
				}
			}
			if (hasAlpha) {
				for (int cy = 0, j = 0; cy < 5; cy++) {
					float fy2 = fy[cy] * 2;
					for (int cx = cy ? 0 : 1; cx < 5 - cy; cx++, j++)
						a += aAc[j] * fx[cx] * fy2;
				}
			}
			float b = l - 2.0f / 3.0f * p;
			float r = (3.0f * l - b + q) / 2.0f;
			float g = r - q;
			auto channel = [](float value) {
				return (uint8_t)std::max(0.0f, std::min(255.0f, value * 255.0f));
			};
			out[x * 4] = channel(b);
			out[x * 4 + 1] = channel(g);
			out[x * 4 + 2] = channel(r);
			out[x * 4 + 3] = channel(a);
		}
	}
	return true;
}

}	// namespace photos
