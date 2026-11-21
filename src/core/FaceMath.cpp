#include "FaceMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace photos {

// docs/FACES.md, "Evaluation", says where these come from.
const float kFaceMinScore = 0.7f;
const float kFaceMinQuality = 0.3f;
const float kSameFaceThreshold = 0.60f;
const float kSameFaceMargin = 0.10f;
const float kCertainFaceThreshold = 0.70f;
const float kMaybeSameFaceThreshold = 0.45f;

const int32_t kFaceStrides[kFaceStrideCount] = { 8, 16, 32 };

// The five points of the 112 x 112 crop face recognition networks of the
// ArcFace family (SFace among them) are trained on.
const float kFaceTemplate[kFaceLandmarkCount][2] = {
	{ 38.2946f, 51.6963f },
	{ 73.5318f, 51.5014f },
	{ 56.0252f, 71.7366f },
	{ 41.5493f, 92.3655f },
	{ 70.7299f, 92.2041f }
};


FaceScanOptions
FaceScanOptions::Default()
{
	return FaceScanOptions();
}


// #pragma mark - windows


static void
tile_positions(int32_t size, int32_t tile, float overlap,
	std::vector<int32_t>& positions)
{
	positions.clear();
	if (size <= tile) {
		positions.push_back(0);
		return;
	}
	int32_t step = std::max((int32_t)(tile * (1 - overlap)), 1);
	int32_t count = (size - tile + step - 1) / step + 1;
	for (int32_t i = 0; i < count; i++) {
		positions.push_back((int32_t)((int64_t)i * (size - tile)
			/ (count - 1)));
	}
}


void
PlanFaceWindows(int32_t width, int32_t height, const int32_t* inputSizes,
	int32_t detectorCount, const FaceScanOptions& options,
	std::vector<FaceWindow>& windows)
{
	windows.clear();
	if (width <= 0 || height <= 0 || detectorCount <= 0)
		return;

	// Whole, at every size that shows noticeably more than the one before:
	// a small input for the faces that fill the picture (which a detector
	// does not find when they are larger than what it was trained on), a
	// large one for the people at the back.
	float shown = 0;
	for (int32_t i = 0; i < detectorCount; i++) {
		FaceWindow whole;
		whole.left = whole.top = 0;
		whole.width = width;
		whole.height = height;
		whole.detector = i;
		whole.whole = true;
		whole.scale = std::min(1.0f,
			(float)inputSizes[i] / std::max(width, height));
		if (i > 0 && whole.scale < shown * 1.3f)
			continue;
		windows.push_back(whole);
		shown = whole.scale;
	}

	// Tiles only when they show the picture noticeably larger than that.
	float scale = std::min(options.tileScale, 1.0f);
	if (scale <= 0 || scale < shown * 1.3f)
		return;

	int32_t detector = detectorCount - 1;
	int32_t tile = (int32_t)(inputSizes[detector] / scale);
	std::vector<int32_t> lefts, tops;
	tile_positions(width, tile, options.tileOverlap, lefts);
	tile_positions(height, tile, options.tileOverlap, tops);
	for (int32_t top : tops) {
		for (int32_t left : lefts) {
			FaceWindow window;
			window.left = left;
			window.top = top;
			window.width = std::min(tile, width);
			window.height = std::min(tile, height);
			window.scale = scale;
			window.detector = detector;
			window.whole = false;
			windows.push_back(window);
		}
	}
}


// #pragma mark - the detector's input


namespace {

// Which source pixels make one destination pixel, and how much of each.
struct Taps {
	std::vector<int32_t> first;
	std::vector<int32_t> count;
	std::vector<float> weights;		// maxCount per destination pixel
	int32_t maxCount;

	Taps(int32_t sourceSize, int32_t destinationSize, float scale)
	{
		maxCount = (int32_t)std::ceil(1 / scale) + 1;
		first.resize(destinationSize);
		count.resize(destinationSize);
		weights.assign((size_t)destinationSize * maxCount, 0);
		for (int32_t i = 0; i < destinationSize; i++) {
			float begin = i / scale;
			float end = std::min((i + 1) / scale, (float)sourceSize);
			int32_t from = std::min((int32_t)begin, sourceSize - 1);
			int32_t to = std::min(std::max((int32_t)std::ceil(end), from + 1),
				std::min(sourceSize, from + maxCount));
			float sum = 0;
			for (int32_t j = from; j < to; j++) {
				float covered = std::min(end, (float)(j + 1))
					- std::max(begin, (float)j);
				covered = std::max(covered, 1e-6f);
				weights[(size_t)i * maxCount + (j - from)] = covered;
				sum += covered;
			}
			for (int32_t j = 0; j < to - from; j++)
				weights[(size_t)i * maxCount + j] /= sum;
			first[i] = from;
			count[i] = to - from;
		}
	}
};

}	// namespace


void
FillDetectorInput(const uint8_t* bits, int32_t width, int32_t height,
	int32_t bytesPerRow, const FaceWindow& window, int32_t inputSize,
	float* tensor)
{
	memset(tensor, 0, (size_t)inputSize * inputSize * 3 * sizeof(float));

	int32_t left = std::max(window.left, 0);
	int32_t top = std::max(window.top, 0);
	int32_t sourceWidth = std::min(window.width, width - left);
	int32_t sourceHeight = std::min(window.height, height - top);
	if (sourceWidth <= 0 || sourceHeight <= 0 || window.scale <= 0)
		return;
	float scale = std::min(window.scale, 1.0f);
	int32_t targetWidth = std::min(inputSize,
		std::max((int32_t)std::lround(sourceWidth * scale), 1));
	int32_t targetHeight = std::min(inputSize,
		std::max((int32_t)std::lround(sourceHeight * scale), 1));

	if (scale == 1) {
		for (int32_t y = 0; y < targetHeight; y++) {
			const uint8_t* source = bits + (size_t)(top + y) * bytesPerRow
				+ (size_t)left * 4;
			float* target = tensor + (size_t)y * inputSize * 3;
			for (int32_t x = 0; x < targetWidth; x++) {
				target[0] = source[0];
				target[1] = source[1];
				target[2] = source[2];
				target += 3;
				source += 4;
			}
		}
		return;
	}

	// Every pixel of the window counts (a face may be a few pixels of a
	// picture ten times the input's size): rows are averaged along x once,
	// then added to the one or two target rows they belong to.
	Taps columns(sourceWidth, targetWidth, scale);
	Taps rows(sourceHeight, targetHeight, scale);
	std::vector<float> line((size_t)targetWidth * 3);
	int32_t lineRow = -1;
	for (int32_t y = 0; y < targetHeight; y++) {
		float* target = tensor + (size_t)y * inputSize * 3;
		for (int32_t j = 0; j < rows.count[y]; j++) {
			int32_t row = rows.first[y] + j;
			float rowWeight = rows.weights[(size_t)y * rows.maxCount + j];
			if (row != lineRow) {
				const uint8_t* source = bits + (size_t)(top + row)
					* bytesPerRow + (size_t)left * 4;
				for (int32_t x = 0; x < targetWidth; x++) {
					const float* weights
						= &columns.weights[(size_t)x * columns.maxCount];
					const uint8_t* pixel = source + (size_t)columns.first[x] * 4;
					float blue = 0, green = 0, red = 0;
					for (int32_t i = 0; i < columns.count[x]; i++) {
						blue += weights[i] * pixel[0];
						green += weights[i] * pixel[1];
						red += weights[i] * pixel[2];
						pixel += 4;
					}
					line[(size_t)x * 3] = blue;
					line[(size_t)x * 3 + 1] = green;
					line[(size_t)x * 3 + 2] = red;
				}
				lineRow = row;
			}
			for (int32_t i = 0; i < targetWidth * 3; i++)
				target[i] += rowWeight * line[i];
		}
	}
}


// #pragma mark - the detector's output


static inline float
clamp01(float value)
{
	// also turns a NaN into 0
	return value > 0 ? (value < 1 ? value : 1) : 0;
}


void
DecodeFaces(const FaceDetectorOutputs& outputs, int32_t inputSize,
	const FaceWindow& window, int32_t pictureWidth, int32_t pictureHeight,
	float minScore, std::vector<FaceCandidate>& candidates)
{
	if (window.scale <= 0)
		return;
	float toPicture = 1 / window.scale;
	// a pixel of the input, in the picture
	float margin = toPicture;
	float right = window.left + window.width;
	float bottom = window.top + window.height;

	for (int32_t level = 0; level < kFaceStrideCount; level++) {
		int32_t stride = kFaceStrides[level];
		int32_t columns = inputSize / stride;
		int32_t cells = columns * columns;
		const float* cls = outputs.cls[level];
		const float* obj = outputs.obj[level];
		const float* bbox = outputs.bbox[level];
		const float* kps = outputs.kps[level];
		if (cls == nullptr || obj == nullptr || bbox == nullptr
			|| kps == nullptr) {
			continue;
		}
		for (int32_t i = 0; i < cells; i++) {
			float score = std::sqrt(clamp01(cls[i]) * clamp01(obj[i]));
			if (!(score >= minScore))
				continue;
			float column = i % columns;
			float row = i / columns;
			const float* box = bbox + (size_t)i * 4;
			const float* points = kps + (size_t)i * 10;
			float centerX = (column + box[0]) * stride;
			float centerY = (row + box[1]) * stride;
			float width = std::exp(std::min(std::max(box[2], -10.0f), 10.0f))
				* stride;
			float height = std::exp(std::min(std::max(box[3], -10.0f), 10.0f))
				* stride;
			if (!std::isfinite(centerX) || !std::isfinite(centerY))
				continue;

			FaceCandidate face;
			face.x0 = window.left + (centerX - width / 2) * toPicture;
			face.y0 = window.top + (centerY - height / 2) * toPicture;
			face.x1 = window.left + (centerX + width / 2) * toPicture;
			face.y1 = window.top + (centerY + height / 2) * toPicture;
			face.score = score;
			bool finite = true;
			for (int32_t n = 0; n < kFaceLandmarkCount; n++) {
				face.landmarks[n][0] = window.left
					+ (points[n * 2] + column) * stride * toPicture;
				face.landmarks[n][1] = window.top
					+ (points[n * 2 + 1] + row) * stride * toPicture;
				finite = finite && std::isfinite(face.landmarks[n][0])
					&& std::isfinite(face.landmarks[n][1]);
			}
			if (!finite)
				continue;

			if (!window.whole) {
				// cut by an edge of the tile inside the picture
				if ((window.left > 0 && face.x0 <= window.left + margin)
					|| (window.top > 0 && face.y0 <= window.top + margin)
					|| (right < pictureWidth && face.x1 >= right - margin)
					|| (bottom < pictureHeight
						&& face.y1 >= bottom - margin)) {
					continue;
				}
			}

			// the box as it lies in the picture
			face.x0 = std::min(std::max(face.x0, 0.0f), (float)pictureWidth);
			face.y0 = std::min(std::max(face.y0, 0.0f), (float)pictureHeight);
			face.x1 = std::min(std::max(face.x1, 0.0f), (float)pictureWidth);
			face.y1 = std::min(std::max(face.y1, 0.0f), (float)pictureHeight);
			if (face.x1 - face.x0 < 1 || face.y1 - face.y0 < 1)
				continue;
			candidates.push_back(face);
		}
	}
}


void
SuppressOverlaps(std::vector<FaceCandidate>& candidates, float maxOverlap,
	int32_t maxFaces)
{
	std::stable_sort(candidates.begin(), candidates.end(),
		[](const FaceCandidate& a, const FaceCandidate& b) {
			return a.score > b.score;
		});
	// what a noisy output could make slow
	if (candidates.size() > 4000)
		candidates.resize(4000);

	std::vector<FaceCandidate> kept;
	for (const FaceCandidate& face : candidates) {
		if ((int32_t)kept.size() >= maxFaces)
			break;
		float area = (face.x1 - face.x0) * (face.y1 - face.y0);
		bool overlaps = false;
		for (const FaceCandidate& better : kept) {
			float width = std::min(face.x1, better.x1)
				- std::max(face.x0, better.x0);
			float height = std::min(face.y1, better.y1)
				- std::max(face.y0, better.y0);
			if (width <= 0 || height <= 0)
				continue;
			float common = width * height;
			float betterArea = (better.x1 - better.x0)
				* (better.y1 - better.y0);
			// the same face at another scale may be a box well inside
			// (or around) the better one
			if (common / (area + betterArea - common) > maxOverlap
				|| common / std::min(area, betterArea) > 0.7f) {
				overlaps = true;
				break;
			}
		}
		if (!overlaps)
			kept.push_back(face);
	}
	candidates.swap(kept);
}


// #pragma mark - alignment


float
FaceTransform::Scale() const
{
	return std::sqrt(a * a + b * b);
}


bool
AlignFace(const float landmarks[kFaceLandmarkCount][2],
	FaceTransform& transform)
{
	// Least squares over rotation, uniform scale and shift (Umeyama's
	// solution in two dimensions, no reflection).
	double meanX = 0, meanY = 0, templateX = 0, templateY = 0;
	for (int32_t i = 0; i < kFaceLandmarkCount; i++) {
		meanX += landmarks[i][0];
		meanY += landmarks[i][1];
		templateX += kFaceTemplate[i][0];
		templateY += kFaceTemplate[i][1];
	}
	meanX /= kFaceLandmarkCount;
	meanY /= kFaceLandmarkCount;
	templateX /= kFaceLandmarkCount;
	templateY /= kFaceLandmarkCount;

	double dot = 0, cross = 0, spread = 0;
	for (int32_t i = 0; i < kFaceLandmarkCount; i++) {
		double x = landmarks[i][0] - meanX;
		double y = landmarks[i][1] - meanY;
		double u = kFaceTemplate[i][0] - templateX;
		double v = kFaceTemplate[i][1] - templateY;
		dot += x * u + y * v;
		cross += x * v - y * u;
		spread += x * x + y * y;
	}
	if (!(spread > 1e-6) || !std::isfinite(dot) || !std::isfinite(cross))
		return false;
	double a = dot / spread;
	double b = cross / spread;
	if (!(a * a + b * b > 1e-12))
		return false;
	transform.a = (float)a;
	transform.b = (float)b;
	transform.tx = (float)(templateX - (a * meanX - b * meanY));
	transform.ty = (float)(templateY - (b * meanX + a * meanY));
	return true;
}


void
WarpFace(const uint8_t* bits, int32_t width, int32_t height,
	int32_t bytesPerRow, const FaceTransform& transform, float* tensor)
{
	// picture = inverse * (crop - shift)
	float squared = transform.a * transform.a + transform.b * transform.b;
	float ia = transform.a / squared;
	float ib = -transform.b / squared;
	// A large face is many pixels per pixel of the crop: several samples
	// per pixel, or fine detail turns into noise.
	float pixels = 1 / std::sqrt(squared);
	int32_t samples = std::min(std::max((int32_t)std::ceil(pixels - 0.25f), 1),
		6);
	float weight = 1.0f / (samples * samples);

	for (int32_t v = 0; v < kFaceCropSize; v++) {
		for (int32_t u = 0; u < kFaceCropSize; u++) {
			float blue = 0, green = 0, red = 0;
			for (int32_t sv = 0; sv < samples; sv++) {
				for (int32_t su = 0; su < samples; su++) {
					float cropX = u + (su + 0.5f) / samples - 0.5f
						- transform.tx;
					float cropY = v + (sv + 0.5f) / samples - 0.5f
						- transform.ty;
					float x = ia * cropX - ib * cropY;
					float y = ib * cropX + ia * cropY;
					float floorX = std::floor(x);
					float floorY = std::floor(y);
					if (!(floorX >= -1 && floorY >= -1 && floorX < width
							&& floorY < height)) {
						continue;
					}
					int32_t x0 = (int32_t)floorX;
					int32_t y0 = (int32_t)floorY;
					float fx = x - floorX;
					float fy = y - floorY;
					for (int32_t corner = 0; corner < 4; corner++) {
						int32_t px = x0 + (corner & 1);
						int32_t py = y0 + (corner >> 1);
						if (px < 0 || py < 0 || px >= width || py >= height)
							continue;
						float w = ((corner & 1) != 0 ? fx : 1 - fx)
							* ((corner >> 1) != 0 ? fy : 1 - fy);
						const uint8_t* pixel = bits + (size_t)py * bytesPerRow
							+ (size_t)px * 4;
						blue += w * pixel[0];
						green += w * pixel[1];
						red += w * pixel[2];
					}
				}
			}
			float* target = tensor + ((size_t)v * kFaceCropSize + u) * 3;
			target[0] = (blue * weight - 127.5f) / 128;
			target[1] = (green * weight - 127.5f) / 128;
			target[2] = (red * weight - 127.5f) / 128;
		}
	}
}


void
FaceCropToBytes(const float* tensor, uint8_t* bgr)
{
	for (int32_t i = 0; i < kFaceCropSize * kFaceCropSize * 3; i++) {
		float value = tensor[i] * 128 + 127.5f;
		bgr[i] = (uint8_t)std::min(std::max(value + 0.5f, 0.0f), 255.0f);
	}
}


// #pragma mark - quality


float
FaceSharpness(const float* tensor)
{
	// The energy of the Laplacian of the middle of the face against that of
	// the picture itself there: fine detail a blurred or enlarged face does
	// not have. Relative, so that a dark or flat face is not "blurred".
	const int32_t kMargin = 20;
	const int32_t size = kFaceCropSize;
	std::vector<float> gray((size_t)size * size);
	for (int32_t i = 0; i < size * size; i++) {
		gray[i] = 0.114f * tensor[i * 3] + 0.587f * tensor[i * 3 + 1]
			+ 0.299f * tensor[i * 3 + 2];
	}
	double mean = 0;
	int32_t count = 0;
	for (int32_t y = kMargin; y < size - kMargin; y++) {
		for (int32_t x = kMargin; x < size - kMargin; x++) {
			mean += gray[y * size + x];
			count++;
		}
	}
	mean /= count;
	double variance = 0, detail = 0;
	for (int32_t y = kMargin; y < size - kMargin; y++) {
		for (int32_t x = kMargin; x < size - kMargin; x++) {
			double value = gray[y * size + x];
			variance += (value - mean) * (value - mean);
			double laplacian = 4 * value - gray[y * size + x - 1]
				- gray[y * size + x + 1] - gray[(y - 1) * size + x]
				- gray[(y + 1) * size + x];
			detail += laplacian * laplacian;
		}
	}
	if (!(variance > 1e-9))
		return 0;
	// A sharp face has a ratio around 0.05 and more, a blurred one 0.01.
	float ratio = (float)(detail / variance);
	return clamp01(std::sqrt(ratio) / 0.22f);
}


float
FaceFrontalness(const float landmarks[kFaceLandmarkCount][2])
{
	// Turned to the side, the nose leaves the middle between the eyes
	// (along the line through them).
	float eyeX = landmarks[1][0] - landmarks[0][0];
	float eyeY = landmarks[1][1] - landmarks[0][1];
	float eyes = std::sqrt(eyeX * eyeX + eyeY * eyeY);
	float mouthX = (landmarks[3][0] + landmarks[4][0]) / 2;
	float mouthY = (landmarks[3][1] + landmarks[4][1]) / 2;
	float middleX = (landmarks[0][0] + landmarks[1][0]) / 2;
	float middleY = (landmarks[0][1] + landmarks[1][1]) / 2;
	float downX = mouthX - middleX;
	float downY = mouthY - middleY;
	float down = std::sqrt(downX * downX + downY * downY);
	if (!(eyes > 1e-3f) || !(down > 1e-3f))
		return 0;
	float aside = ((landmarks[2][0] - middleX) * eyeX
		+ (landmarks[2][1] - middleY) * eyeY) / (eyes * eyes);
	// In a profile the eyes also come close together against the distance
	// from eyes to mouth (0.87 in the template).
	float narrow = eyes / down;
	float turn = clamp01(1 - std::fabs(aside) / 0.5f);
	float wide = clamp01(narrow / 0.6f);
	return turn * wide;
}


float
FaceQuality(const FaceCandidate& face, float sharpness)
{
	// What was measured to go with faces being told apart less well: few
	// pixels (below some ten between the eyes there is no telling who it
	// is, from twenty on more add little), a head turned away, blur, and a
	// detector that was not sure (which is what it is of faces half hidden).
	float eyeX = face.landmarks[1][0] - face.landmarks[0][0];
	float eyeY = face.landmarks[1][1] - face.landmarks[0][1];
	float eyes = std::sqrt(eyeX * eyeX + eyeY * eyeY);
	float size = clamp01((eyes - 8) / 12);
	float frontal = 0.4f
		+ 0.6f * clamp01(FaceFrontalness(face.landmarks) / 0.7f);
	float sharp = 0.4f + 0.6f * clamp01((sharpness - 0.2f) / 0.5f);
	float sure = 0.3f + 0.7f * clamp01((face.score - 0.7f) / 0.2f);
	return size * frontal * sharp * sure;
}


// #pragma mark - embeddings


void
NormalizeL2(float* values, int32_t count)
{
	double sum = 0;
	for (int32_t i = 0; i < count; i++)
		sum += (double)values[i] * values[i];
	if (!(sum > 1e-24) || !std::isfinite(sum))
		return;
	float scale = (float)(1 / std::sqrt(sum));
	for (int32_t i = 0; i < count; i++)
		values[i] *= scale;
}


float
CosineSimilarity(const float* a, const float* b, int32_t count)
{
	double dot = 0, lengthA = 0, lengthB = 0;
	for (int32_t i = 0; i < count; i++) {
		dot += (double)a[i] * b[i];
		lengthA += (double)a[i] * a[i];
		lengthB += (double)b[i] * b[i];
	}
	if (!(lengthA > 1e-24) || !(lengthB > 1e-24))
		return 0;
	return (float)(dot / std::sqrt(lengthA * lengthB));
}


FaceMatch
ClassifyFaceMatch(float best, float nextBest)
{
	if (!(best >= kMaybeSameFaceThreshold))
		return kFaceNoMatch;
	if (best >= kCertainFaceThreshold
		|| (best >= kSameFaceThreshold
			&& best - std::max(nextBest, 0.0f) >= kSameFaceMargin)) {
		return kFaceSameMatch;
	}
	return kFaceMaybeMatch;
}


// #pragma mark - the whole of it


bool
FindFaces(FaceNetworks& networks, const uint8_t* bits, int32_t width,
	int32_t height, int32_t bytesPerRow, const FaceScanOptions& options,
	std::vector<DetectedFace>& faces, std::vector<FaceCandidate>* pixels)
{
	faces.clear();
	if (pixels != nullptr)
		pixels->clear();
	if (bits == nullptr || width <= 0 || height <= 0
		|| bytesPerRow < width * 4 || networks.CountDetectors() < 1) {
		return false;
	}

	const FaceScanOptions& used = options;
	float minScore = used.minScore >= 0 ? used.minScore : kFaceMinScore;
	float minQuality = used.minQuality >= 0 ? used.minQuality
		: kFaceMinQuality;

	std::vector<int32_t> sizes;
	for (int32_t i = 0; i < networks.CountDetectors(); i++)
		sizes.push_back(networks.DetectorInputSize(i));
	std::vector<FaceWindow> windows;
	PlanFaceWindows(width, height, sizes.data(), (int32_t)sizes.size(), used,
		windows);

	std::vector<FaceCandidate> candidates;
	for (const FaceWindow& window : windows) {
		int32_t size = sizes[window.detector];
		float* input = networks.DetectorInput(window.detector);
		if (input == nullptr)
			return false;
		FillDetectorInput(bits, width, height, bytesPerRow, window, size,
			input);
		FaceDetectorOutputs outputs;
		if (!networks.RunDetector(window.detector, outputs))
			return false;
		DecodeFaces(outputs, size, window, width, height, minScore,
			candidates);
	}
	SuppressOverlaps(candidates, 0.3f, used.maxFaces);

	int32_t embeddingSize = networks.EmbeddingSize();
	for (const FaceCandidate& candidate : candidates) {
		DetectedFace face;
		face.x = candidate.x0 / width;
		face.y = candidate.y0 / height;
		face.width = (candidate.x1 - candidate.x0) / width;
		face.height = (candidate.y1 - candidate.y0) / height;
		face.score = candidate.score;
		face.quality = 0;
		for (int32_t i = 0; i < kFaceLandmarkCount; i++) {
			face.landmarks[i][0] = candidate.landmarks[i][0] / width;
			face.landmarks[i][1] = candidate.landmarks[i][1] / height;
		}

		FaceTransform transform;
		float* crop = networks.EmbedderInput();
		if (crop != nullptr && AlignFace(candidate.landmarks, transform)) {
			WarpFace(bits, width, height, bytesPerRow, transform, crop);
			face.quality = FaceQuality(candidate, FaceSharpness(crop));
			if (used.embed && embeddingSize > 0
				&& face.quality >= minQuality) {
				const float* embedding = networks.RunEmbedder();
				if (embedding == nullptr)
					return false;
				face.embedding.assign(embedding, embedding + embeddingSize);
				NormalizeL2(face.embedding.data(), embeddingSize);
			}
		}
		faces.push_back(face);
		if (pixels != nullptr)
			pixels->push_back(candidate);
	}
	return true;
}

}	// namespace photos
