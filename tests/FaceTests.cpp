// core/FaceMath: everything around the face networks, with made-up network
// outputs.
#include "Test.h"

#include "core/FaceMath.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace photos;

namespace {

const int32_t kSizes[] = { 640, 1280 };


// A B_RGB32-layout picture filled by a function of x and y.
struct Picture {
	int32_t width, height;
	std::vector<uint8_t> bits;

	template<typename Function>
	Picture(int32_t width, int32_t height, Function color)
		:
		width(width),
		height(height),
		bits((size_t)width * height * 4)
	{
		for (int32_t y = 0; y < height; y++) {
			for (int32_t x = 0; x < width; x++) {
				uint8_t* pixel = &bits[((size_t)y * width + x) * 4];
				color(x, y, pixel);
				pixel[3] = 255;
			}
		}
	}
};


bool
near(float a, float b, float tolerance)
{
	return std::fabs(a - b) <= tolerance;
}


// The detector's outputs for a 640 input with nothing in them.
struct Outputs {
	std::vector<float> cls[3], obj[3], bbox[3], kps[3];
	FaceDetectorOutputs pointers;

	Outputs(int32_t inputSize = 640)
	{
		for (int32_t i = 0; i < 3; i++) {
			int32_t cells = (inputSize / kFaceStrides[i])
				* (inputSize / kFaceStrides[i]);
			cls[i].assign(cells, 0);
			obj[i].assign(cells, 0);
			bbox[i].assign(cells * 4, 0);
			kps[i].assign(cells * 10, 0);
			pointers.cls[i] = cls[i].data();
			pointers.obj[i] = obj[i].data();
			pointers.bbox[i] = bbox[i].data();
			pointers.kps[i] = kps[i].data();
		}
	}

	// A face whose box has its centre in the middle of a cell, with the
	// template's landmarks scaled into the box.
	void Put(int32_t level, int32_t column, int32_t row, float width,
		float height, float score, int32_t inputSize = 640)
	{
		int32_t stride = kFaceStrides[level];
		int32_t index = row * (inputSize / stride) + column;
		cls[level][index] = score;
		obj[level][index] = score;
		bbox[level][index * 4] = 0.5f;
		bbox[level][index * 4 + 1] = 0.5f;
		bbox[level][index * 4 + 2] = std::log(width / stride);
		bbox[level][index * 4 + 3] = std::log(height / stride);
		for (int32_t n = 0; n < kFaceLandmarkCount; n++) {
			// template coordinates (0..112) into the box around the centre
			float x = (kFaceTemplate[n][0] / 112 - 0.5f) * width;
			float y = (kFaceTemplate[n][1] / 112 - 0.5f) * height;
			kps[level][index * 10 + n * 2] = 0.5f + x / stride;
			kps[level][index * 10 + n * 2 + 1] = 0.5f + y / stride;
		}
	}
};


FaceWindow
whole_window(int32_t width, int32_t height, float scale)
{
	FaceWindow window;
	window.left = window.top = 0;
	window.width = width;
	window.height = height;
	window.scale = scale;
	window.detector = 0;
	window.whole = true;
	return window;
}

}	// namespace


TEST(FaceWindowsSmallPictureIsOneWindow)
{
	std::vector<FaceWindow> windows;
	FaceScanOptions options;
	PlanFaceWindows(600, 400, kSizes, 2, options, windows);
	CHECK_EQ(windows.size(), (size_t)1);
	if (windows.size() != 1)
		return;
	CHECK(windows[0].whole);
	CHECK_EQ(windows[0].detector, 0);
	CHECK_EQ(windows[0].scale, 1.0f);
	CHECK_EQ(windows[0].width, 600);
	CHECK_EQ(windows[0].height, 400);

	PlanFaceWindows(0, 400, kSizes, 2, options, windows);
	CHECK(windows.empty());
	PlanFaceWindows(600, 400, kSizes, 0, options, windows);
	CHECK(windows.empty());
}


TEST(FaceWindowsWholeAtEverySizeThatShowsMore)
{
	std::vector<FaceWindow> windows;
	FaceScanOptions options;
	// a preview: once into each input, no tiles
	PlanFaceWindows(2000, 1500, kSizes, 2, options, windows);
	CHECK_EQ(windows.size(), (size_t)2);
	if (windows.size() != 2)
		return;
	CHECK(windows[0].whole && windows[1].whole);
	CHECK_EQ(windows[0].detector, 0);
	CHECK(near(windows[0].scale, 0.32f, 1e-6f));
	CHECK_EQ(windows[1].detector, 1);
	CHECK(near(windows[1].scale, 0.64f, 1e-6f));

	// a little larger than the small input: the large one adds too little
	PlanFaceWindows(800, 600, kSizes, 2, options, windows);
	CHECK_EQ(windows.size(), (size_t)1);
	PlanFaceWindows(900, 600, kSizes, 2, options, windows);
	CHECK_EQ(windows.size(), (size_t)2);
	if (windows.size() == 2)
		CHECK_EQ(windows[1].scale, 1.0f);

	// a camera's picture: tiles of the large input as well
	PlanFaceWindows(6000, 4000, kSizes, 2, options, windows);
	CHECK_EQ(windows.size(), (size_t)8);
	if (windows.size() != 8)
		return;
	CHECK(windows[1].whole && !windows[2].whole);
	CHECK_EQ(windows[2].detector, 1);
	CHECK_EQ(windows[2].scale, 0.5f);
	CHECK_EQ(windows[2].width, 2560);
	options.tileScale = 0;
	PlanFaceWindows(6000, 4000, kSizes, 2, options, windows);
	CHECK_EQ(windows.size(), (size_t)2);
}


TEST(FaceWindowsTilesCoverThePicture)
{
	std::vector<FaceWindow> windows;
	FaceScanOptions options;
	options.tileScale = 0.5f;
	PlanFaceWindows(2000, 1500, kSizes, 1, options, windows);
	// the whole picture, then 2 x 2 tiles of 1280 picture pixels
	CHECK_EQ(windows.size(), (size_t)5);
	if (windows.size() != 5)
		return;
	CHECK(windows[0].whole);
	CHECK(near(windows[0].scale, 0.32f, 1e-6f));
	std::vector<int> covered(2000 * 1500, 0);
	for (size_t i = 1; i < windows.size(); i++) {
		const FaceWindow& tile = windows[i];
		CHECK(!tile.whole);
		CHECK_EQ(tile.scale, 0.5f);
		CHECK(tile.left >= 0 && tile.top >= 0);
		CHECK(tile.left + tile.width <= 2000);
		CHECK(tile.top + tile.height <= 1500);
		CHECK(tile.width * tile.scale <= 640 && tile.height * tile.scale <= 640);
		for (int32_t y = tile.top; y < tile.top + tile.height; y++) {
			for (int32_t x = tile.left; x < tile.left + tile.width; x++)
				covered[y * 2000 + x]++;
		}
	}
	bool all = true;
	for (int count : covered)
		all = all && count > 0;
	CHECK(all);
	// neighbours share at least a quarter of a tile
	CHECK(windows[2].left - windows[1].left <= 1280 * 3 / 4);

	// no tiles when they would show little more than the whole picture
	options.tileScale = 0.4f;
	PlanFaceWindows(2000, 1500, kSizes, 1, options, windows);
	CHECK_EQ(windows.size(), (size_t)1);
	options.tileScale = 0;
	PlanFaceWindows(6000, 4000, kSizes, 1, options, windows);
	CHECK_EQ(windows.size(), (size_t)1);

	// tiles are never enlarged
	options.tileScale = 3;
	PlanFaceWindows(2000, 1500, kSizes, 1, options, windows);
	CHECK(windows.size() > 1);
	if (windows.size() > 1) {
		CHECK_EQ(windows[1].scale, 1.0f);
		CHECK_EQ(windows[1].width, 640);
	}
}


TEST(FaceDetectorInputCopiesAndAverages)
{
	Picture picture(100, 60, [](int32_t x, int32_t y, uint8_t* pixel) {
		pixel[0] = x;
		pixel[1] = y;
		pixel[2] = 200;
	});
	std::vector<float> tensor(64 * 64 * 3, -1);

	// as it is: the window's pixels in order, black around them
	FaceWindow window = whole_window(100, 60, 1);
	window.left = 10;
	window.top = 5;
	window.width = 40;
	window.height = 30;
	FillDetectorInput(picture.bits.data(), 100, 60, 400, window, 64,
		tensor.data());
	CHECK_EQ(tensor[0], 10.0f);
	CHECK_EQ(tensor[1], 5.0f);
	CHECK_EQ(tensor[2], 200.0f);
	CHECK_EQ(tensor[(29 * 64 + 39) * 3], 49.0f);
	CHECK_EQ(tensor[(29 * 64 + 39) * 3 + 1], 34.0f);
	CHECK_EQ(tensor[(29 * 64 + 40) * 3], 0.0f);
	CHECK_EQ(tensor[(30 * 64 + 39) * 3 + 2], 0.0f);

	// at half the size every value is the mean of four pixels
	window = whole_window(100, 60, 0.5f);
	FillDetectorInput(picture.bits.data(), 100, 60, 400, window, 64,
		tensor.data());
	CHECK(near(tensor[0], 0.5f, 1e-4f));
	CHECK(near(tensor[1], 0.5f, 1e-4f));
	CHECK(near(tensor[(10 * 64 + 20) * 3], 40.5f, 1e-3f));
	CHECK(near(tensor[(10 * 64 + 20) * 3 + 1], 20.5f, 1e-3f));
	CHECK(near(tensor[(10 * 64 + 20) * 3 + 2], 200, 1e-3f));
	CHECK_EQ(tensor[(10 * 64 + 50) * 3 + 2], 0.0f);
	CHECK_EQ(tensor[(30 * 64 + 20) * 3 + 2], 0.0f);

	// a scale that is no whole number keeps the mean of the picture
	Picture checker(90, 90, [](int32_t x, int32_t y, uint8_t* pixel) {
		pixel[0] = pixel[1] = pixel[2] = ((x + y) & 1) != 0 ? 255 : 0;
	});
	window = whole_window(90, 90, 64 / 90.0f);
	FillDetectorInput(checker.bits.data(), 90, 90, 360, window, 64,
		tensor.data());
	double sum = 0;
	for (int32_t i = 0; i < 64 * 64 * 3; i++)
		sum += tensor[i];
	CHECK(near(sum / (64 * 64 * 3), 127.5f, 0.5f));
	float lowest = 255, highest = 0;
	for (int32_t i = 0; i < 64 * 64 * 3; i++) {
		lowest = std::min(lowest, tensor[i]);
		highest = std::max(highest, tensor[i]);
	}
	CHECK(lowest >= 0 && highest <= 255.01f);

	// a window that leaves the picture takes what there is
	window = whole_window(100, 60, 1);
	window.left = 90;
	window.width = 64;
	FillDetectorInput(picture.bits.data(), 100, 60, 400, window, 64,
		tensor.data());
	CHECK_EQ(tensor[9 * 3], 99.0f);
	CHECK_EQ(tensor[10 * 3 + 2], 0.0f);
}


TEST(FaceDecodeMapsCellsToThePicture)
{
	Outputs outputs;
	// stride 8, cell (10, 20): centre (84, 164) in the input
	outputs.Put(0, 10, 20, 32, 40, 0.81f);
	// stride 32, cell (5, 5): centre (176, 176)
	outputs.Put(2, 5, 5, 128, 160, 0.64f);
	// below the threshold
	outputs.Put(1, 5, 5, 60, 60, 0.3f);

	FaceWindow window = whole_window(1280, 1280, 0.5f);
	window.left = 100;
	window.top = 50;
	window.whole = false;
	window.width = 1280;
	window.height = 1280;
	std::vector<FaceCandidate> faces;
	DecodeFaces(outputs.pointers, 640, window, 4000, 3000, 0.6f, faces);
	CHECK_EQ(faces.size(), (size_t)2);
	if (faces.size() != 2)
		return;
	const FaceCandidate& small = faces[0];
	CHECK(near(small.score, 0.81f, 1e-5f));
	CHECK(near(small.x0, 100 + (84 - 16) * 2, 1e-2f));
	CHECK(near(small.y0, 50 + (164 - 20) * 2, 1e-2f));
	CHECK(near(small.x1, 100 + (84 + 16) * 2, 1e-2f));
	CHECK(near(small.y1, 50 + (164 + 20) * 2, 1e-2f));
	// the nose of the template sits at (56.03, 71.74) of 112
	CHECK(near(small.landmarks[2][0],
		100 + (84 + (56.0252f / 112 - 0.5f) * 32) * 2, 1e-2f));
	CHECK(near(small.landmarks[2][1],
		50 + (164 + (71.7366f / 112 - 0.5f) * 40) * 2, 1e-2f));
	// right eye left of the left eye
	CHECK(small.landmarks[0][0] < small.landmarks[1][0]);
	const FaceCandidate& large = faces[1];
	CHECK(near(large.score, 0.64f, 1e-5f));
	CHECK(near(large.x1 - large.x0, 256, 1e-2f));
	CHECK(near(large.y1 - large.y0, 320, 1e-2f));

	// a lower threshold lets the third in
	faces.clear();
	DecodeFaces(outputs.pointers, 640, window, 4000, 3000, 0.25f, faces);
	CHECK_EQ(faces.size(), (size_t)3);
}


TEST(FaceDecodeDropsFacesCutByATile)
{
	// a face against the right edge of the input: columns 76..79 of stride 8
	Outputs outputs;
	outputs.Put(0, 78, 40, 32, 32, 0.9f);		// 608..640: touches
	outputs.Put(0, 40, 40, 32, 32, 0.9f);		// in the middle
	FaceWindow tile = whole_window(640, 640, 1);
	tile.whole = false;
	tile.left = 0;
	tile.top = 0;

	// the tile ends inside the picture: the face at its edge is cut
	std::vector<FaceCandidate> faces;
	DecodeFaces(outputs.pointers, 640, tile, 2000, 640, 0.6f, faces);
	CHECK_EQ(faces.size(), (size_t)1);
	if (faces.size() != 1)
		return;
	CHECK(near(faces[0].x0, 308, 1e-2f));

	// the tile's edge is the picture's: nothing more to see of it
	faces.clear();
	DecodeFaces(outputs.pointers, 640, tile, 640, 640, 0.6f, faces);
	CHECK_EQ(faces.size(), (size_t)2);

	// the whole picture keeps everything, clipped to the picture
	Outputs over;
	over.Put(0, 79, 40, 64, 64, 0.9f);			// 604..668
	faces.clear();
	DecodeFaces(over.pointers, 640, whole_window(640, 640, 1), 640, 640,
		0.6f, faces);
	CHECK_EQ(faces.size(), (size_t)1);
	if (faces.size() != 1)
		return;
	CHECK_EQ(faces[0].x1, 640.0f);
}


TEST(FaceDecodeSurvivesNonsense)
{
	Outputs outputs;
	outputs.Put(0, 10, 10, 32, 32, 0.9f);
	outputs.cls[0][5] = NAN;
	outputs.obj[0][5] = 1;
	outputs.cls[0][6] = 5;				// clamped to 1
	outputs.obj[0][6] = 1;
	outputs.bbox[0][6 * 4 + 2] = 1e9f;	// a width of e^10 cells at most
	outputs.cls[0][7] = 1;
	outputs.obj[0][7] = 1;
	outputs.bbox[0][7 * 4] = INFINITY;
	outputs.cls[0][8] = 1;
	outputs.obj[0][8] = 1;
	outputs.kps[0][8 * 10 + 3] = NAN;
	std::vector<FaceCandidate> faces;
	DecodeFaces(outputs.pointers, 640, whole_window(640, 640, 1), 640, 640,
		0.6f, faces);
	CHECK_EQ(faces.size(), (size_t)2);
	for (const FaceCandidate& face : faces) {
		CHECK(std::isfinite(face.x0) && std::isfinite(face.x1));
		CHECK(face.x0 >= 0 && face.x1 <= 640 && face.y0 >= 0 && face.y1 <= 640);
	}

	// missing outputs are nobody
	FaceDetectorOutputs none;
	memset(&none, 0, sizeof(none));
	faces.clear();
	DecodeFaces(none, 640, whole_window(640, 640, 1), 640, 640, 0.6f, faces);
	CHECK(faces.empty());
}


static FaceCandidate
box(float x0, float y0, float x1, float y1, float score)
{
	FaceCandidate face;
	memset(&face, 0, sizeof(face));
	face.x0 = x0;
	face.y0 = y0;
	face.x1 = x1;
	face.y1 = y1;
	face.score = score;
	return face;
}


TEST(FaceSuppressionKeepsTheBestOfOverlaps)
{
	std::vector<FaceCandidate> faces;
	faces.push_back(box(100, 100, 200, 200, 0.7f));
	faces.push_back(box(105, 102, 204, 203, 0.9f));	// the same face, better
	faces.push_back(box(400, 100, 500, 200, 0.8f));	// another one
	faces.push_back(box(120, 120, 170, 170, 0.75f));	// inside the first
	faces.push_back(box(190, 100, 290, 200, 0.65f));	// a neighbour, touching
	SuppressOverlaps(faces);
	CHECK_EQ(faces.size(), (size_t)3);
	if (faces.size() != 3)
		return;
	CHECK_EQ(faces[0].score, 0.9f);
	CHECK_EQ(faces[1].score, 0.8f);
	CHECK_EQ(faces[2].score, 0.65f);

	faces.clear();
	for (int32_t i = 0; i < 50; i++)
		faces.push_back(box(i * 100, 0, i * 100 + 50, 50, 0.6f + i * 0.001f));
	SuppressOverlaps(faces, 0.3f, 10);
	CHECK_EQ(faces.size(), (size_t)10);
	CHECK(near(faces[0].score, 0.649f, 1e-6f));
}


TEST(FaceAlignmentFindsTheSimilarity)
{
	// the template turned by 30 degrees, enlarged 2.5 times and moved
	const float angle = 30 * M_PI / 180;
	const float scale = 2.5f;
	float landmarks[kFaceLandmarkCount][2];
	for (int32_t i = 0; i < kFaceLandmarkCount; i++) {
		float x = kFaceTemplate[i][0];
		float y = kFaceTemplate[i][1];
		landmarks[i][0] = scale * (std::cos(angle) * x - std::sin(angle) * y)
			+ 700;
		landmarks[i][1] = scale * (std::sin(angle) * x + std::cos(angle) * y)
			+ 300;
	}
	FaceTransform transform;
	CHECK(AlignFace(landmarks, transform));
	CHECK(near(transform.Scale(), 1 / scale, 1e-4f));
	for (int32_t i = 0; i < kFaceLandmarkCount; i++) {
		float x = transform.a * landmarks[i][0] - transform.b * landmarks[i][1]
			+ transform.tx;
		float y = transform.b * landmarks[i][0] + transform.a * landmarks[i][1]
			+ transform.ty;
		CHECK(near(x, kFaceTemplate[i][0], 1e-2f));
		CHECK(near(y, kFaceTemplate[i][1], 1e-2f));
	}

	// landmarks that are no face: the closest fit, and no reflection
	float mirrored[kFaceLandmarkCount][2];
	for (int32_t i = 0; i < kFaceLandmarkCount; i++) {
		mirrored[i][0] = 112 - kFaceTemplate[i][0];
		mirrored[i][1] = kFaceTemplate[i][1];
	}
	CHECK(AlignFace(mirrored, transform));
	CHECK(std::isfinite(transform.a) && std::isfinite(transform.tx));

	float point[kFaceLandmarkCount][2];
	for (int32_t i = 0; i < kFaceLandmarkCount; i++)
		point[i][0] = point[i][1] = 50;
	CHECK(!AlignFace(point, transform));
	point[0][0] = NAN;
	CHECK(!AlignFace(point, transform));
}


TEST(FaceWarpSamplesThePicture)
{
	Picture picture(400, 300, [](int32_t x, int32_t y, uint8_t* pixel) {
		pixel[0] = x / 2;
		pixel[1] = y / 2;
		pixel[2] = 77;
	});
	std::vector<float> crop(kFaceCropSize * kFaceCropSize * 3);

	// shifted only: crop (u, v) is picture (u + 10, v + 20)
	FaceTransform shift = { 1, 0, -10, -20 };
	WarpFace(picture.bits.data(), 400, 300, 1600, shift, crop.data());
	std::vector<uint8_t> bytes(crop.size());
	FaceCropToBytes(crop.data(), bytes.data());
	CHECK_EQ((int)bytes[0], 5);
	CHECK_EQ((int)bytes[1], 10);
	CHECK_EQ((int)bytes[2], 77);
	CHECK_EQ((int)bytes[(50 * 112 + 100) * 3], 55);
	CHECK_EQ((int)bytes[(50 * 112 + 100) * 3 + 1], 35);
	CHECK(near(crop[2], (77 - 127.5f) / 128, 1e-5f));

	// a quarter of the size: every pixel is the mean of what it covers
	FaceTransform quarter = { 0.25f, 0, 0, 0 };
	WarpFace(picture.bits.data(), 400, 300, 1600, quarter, crop.data());
	// crop (10, 10) covers picture 38.5 .. 42.5: blue is x / 2, 19 to 21
	float blue = crop[(10 * 112 + 10) * 3] * 128 + 127.5f;
	CHECK(near(blue, 20, 0.6f));
	// beyond the picture (x >= 400 is crop u >= 100) it is black
	CHECK(near(crop[(10 * 112 + 105) * 3 + 2], -127.5f / 128, 1e-5f));
	CHECK(near(crop[(80 * 112 + 10) * 3 + 2], -127.5f / 128, 1e-5f));

	// turned by a quarter turn: crop x runs along picture y
	FaceTransform turned = { 0, 1, 150, 0 };
	WarpFace(picture.bits.data(), 400, 300, 1600, turned, crop.data());
	FaceCropToBytes(crop.data(), bytes.data());
	// crop (u, v) = (150 - y, x): u = 50, v = 60 is picture (60, 100)
	CHECK_EQ((int)bytes[(60 * 112 + 50) * 3], 30);
	CHECK_EQ((int)bytes[(60 * 112 + 50) * 3 + 1], 50);
}


TEST(FaceSharpnessTellsBlurFromDetail)
{
	std::vector<float> sharp(kFaceCropSize * kFaceCropSize * 3);
	uint32_t state = 7;
	for (int32_t i = 0; i < kFaceCropSize * kFaceCropSize; i++) {
		state = state * 1664525 + 1013904223;
		float value = ((state >> 24) / 255.0f - 0.5f);
		sharp[i * 3] = sharp[i * 3 + 1] = sharp[i * 3 + 2] = value;
	}
	// the same, averaged over 9 x 9 three times: a face out of focus
	std::vector<float> blurred(sharp);
	for (int32_t pass = 0; pass < 3; pass++) {
		std::vector<float> source(blurred);
		for (int32_t y = 4; y < kFaceCropSize - 4; y++) {
			for (int32_t x = 4; x < kFaceCropSize - 4; x++) {
				float sum = 0;
				for (int32_t dy = -4; dy <= 4; dy++) {
					for (int32_t dx = -4; dx <= 4; dx++)
						sum += source[((y + dy) * kFaceCropSize + x + dx) * 3];
				}
				for (int32_t c = 0; c < 3; c++)
					blurred[(y * kFaceCropSize + x) * 3 + c] = sum / 81;
			}
		}
	}
	std::vector<float> flat(sharp.size(), 0.3f);
	float sharpness = FaceSharpness(sharp.data());
	float blurredness = FaceSharpness(blurred.data());
	CHECK(sharpness > 0.9f);
	CHECK(blurredness < sharpness);
	CHECK_EQ(FaceSharpness(flat.data()), 0.0f);
	// darker or lower in contrast is not less sharp
	std::vector<float> dim(sharp);
	for (float& value : dim)
		value = value * 0.2f - 0.5f;
	CHECK(near(FaceSharpness(dim.data()), sharpness, 1e-3f));
}


TEST(FaceQualityRatesSizeAndPose)
{
	FaceCandidate face = box(0, 0, 112, 112, 0.9f);
	memcpy(face.landmarks, kFaceTemplate, sizeof(face.landmarks));
	CHECK(FaceFrontalness(face.landmarks) > 0.9f);
	float good = FaceQuality(face, 1);
	CHECK(good > 0.9f);
	CHECK(FaceQuality(face, 0.2f) < good);

	// a tenth of the size: eyes 3.5 pixels apart
	FaceCandidate tiny = face;
	for (int32_t i = 0; i < kFaceLandmarkCount; i++) {
		tiny.landmarks[i][0] *= 0.1f;
		tiny.landmarks[i][1] *= 0.1f;
	}
	CHECK(near(FaceFrontalness(tiny.landmarks),
		FaceFrontalness(face.landmarks), 1e-4f));
	CHECK_EQ(FaceQuality(tiny, 1), 0.0f);

	// a profile: the nose next to an eye, the eyes close together
	FaceCandidate profile = face;
	profile.landmarks[2][0] = face.landmarks[1][0] + 4;
	CHECK(FaceFrontalness(profile.landmarks) < 0.2f);
	CHECK(FaceQuality(profile, 1) < 0.5f * good);

	// turned on its side it is as frontal as upright
	FaceCandidate lying = face;
	for (int32_t i = 0; i < kFaceLandmarkCount; i++) {
		lying.landmarks[i][0] = 112 - face.landmarks[i][1];
		lying.landmarks[i][1] = face.landmarks[i][0];
	}
	CHECK(near(FaceFrontalness(lying.landmarks),
		FaceFrontalness(face.landmarks), 1e-4f));

	// a weak detection counts for less
	FaceCandidate unsure = face;
	unsure.score = 0.75f;
	CHECK(FaceQuality(unsure, 1) < good);

	float degenerate[kFaceLandmarkCount][2] = {};
	CHECK_EQ(FaceFrontalness(degenerate), 0.0f);
}


TEST(FaceEmbeddingArithmetic)
{
	float a[4] = { 3, 0, 4, 0 };
	float b[4] = { 0, 2, 0, 0 };
	float c[4] = { 6, 0, 8, 0 };
	CHECK(near(CosineSimilarity(a, c, 4), 1, 1e-6f));
	CHECK(near(CosineSimilarity(a, b, 4), 0, 1e-6f));
	NormalizeL2(a, 4);
	CHECK(near(a[0], 0.6f, 1e-6f));
	CHECK(near(a[2], 0.8f, 1e-6f));
	float zero[4] = { 0, 0, 0, 0 };
	NormalizeL2(zero, 4);
	CHECK_EQ(zero[0], 0.0f);
	CHECK_EQ(CosineSimilarity(zero, a, 4), 0.0f);
	float negative[4] = { -0.6f, 0, -0.8f, 0 };
	CHECK(near(CosineSimilarity(a, negative, 4), -1, 1e-6f));

	// the thresholds are in a sensible order
	CHECK(kMaybeSameFaceThreshold < kSameFaceThreshold);
	CHECK(kSameFaceThreshold < kCertainFaceThreshold);
	CHECK(kCertainFaceThreshold < 1 && kMaybeSameFaceThreshold > 0);
	CHECK(kSameFaceMargin > 0);
	CHECK(kFaceMinScore > 0 && kFaceMinScore < 1);
	CHECK(kFaceMinQuality > 0 && kFaceMinQuality < 1);
}


TEST(FaceMatchesAreClassified)
{
	// nobody near
	CHECK_EQ((int)ClassifyFaceMatch(0.2f, -1), (int)kFaceNoMatch);
	CHECK_EQ((int)ClassifyFaceMatch(NAN, -1), (int)kFaceNoMatch);
	CHECK_EQ((int)ClassifyFaceMatch(kMaybeSameFaceThreshold - 0.01f, -1),
		(int)kFaceNoMatch);
	// somebody, perhaps
	CHECK_EQ((int)ClassifyFaceMatch(kMaybeSameFaceThreshold, -1),
		(int)kFaceMaybeMatch);
	CHECK_EQ((int)ClassifyFaceMatch(kSameFaceThreshold - 0.01f, 0.1f),
		(int)kFaceMaybeMatch);
	// close, and nobody else is
	CHECK_EQ((int)ClassifyFaceMatch(kSameFaceThreshold + 0.01f, -1),
		(int)kFaceSameMatch);
	CHECK_EQ((int)ClassifyFaceMatch(kSameFaceThreshold + 0.01f,
		kSameFaceThreshold - kSameFaceMargin), (int)kFaceSameMatch);
	// close, but so is someone else
	CHECK_EQ((int)ClassifyFaceMatch(kSameFaceThreshold + 0.02f,
		kSameFaceThreshold - 0.02f), (int)kFaceMaybeMatch);
	// so close that it does not matter who else is
	CHECK_EQ((int)ClassifyFaceMatch(kCertainFaceThreshold + 0.01f,
		kCertainFaceThreshold), (int)kFaceSameMatch);
}


namespace {

// Networks that "find" one face wherever the picture is, and whose
// embedding is the mean colour of the crop they are given.
class FakeNetworks : public FaceNetworks {
public:
	Outputs outputs;
	std::vector<float> input;
	std::vector<float> crop;
	float embedding[4];
	int detectorRuns = 0;
	int embedderRuns = 0;
	bool failDetector = false;
	bool failEmbedder = false;

	FakeNetworks()
		:
		input(640 * 640 * 3),
		crop(kFaceCropSize * kFaceCropSize * 3)
	{
	}

	virtual int32_t CountDetectors() const { return 1; }
	virtual int32_t DetectorInputSize(int32_t) const { return 640; }
	virtual float* DetectorInput(int32_t) { return input.data(); }
	virtual bool RunDetector(int32_t, FaceDetectorOutputs& result)
	{
		detectorRuns++;
		result = outputs.pointers;
		return !failDetector;
	}
	virtual int32_t EmbeddingSize() const { return 4; }
	virtual float* EmbedderInput() { return crop.data(); }
	virtual const float* RunEmbedder()
	{
		embedderRuns++;
		if (failEmbedder)
			return nullptr;
		double sums[3] = { 0, 0, 0 };
		for (int32_t i = 0; i < kFaceCropSize * kFaceCropSize; i++) {
			for (int32_t c = 0; c < 3; c++)
				sums[c] += crop[i * 3 + c] * 128 + 127.5;
		}
		for (int32_t c = 0; c < 3; c++)
			embedding[c] = sums[c] / (kFaceCropSize * kFaceCropSize);
		embedding[3] = 0;
		return embedding;
	}
};

}	// namespace


TEST(FaceFindingRunsTheWholePipeline)
{
	// a picture with texture (or the face would count as blurred), mostly
	// blue 40, green 80, red 160
	Picture picture(640, 480, [](int32_t x, int32_t y, uint8_t* pixel) {
		int32_t texture = ((x * 7 + y * 13) % 5) * 8;
		pixel[0] = 40 + texture;
		pixel[1] = 80 + texture;
		pixel[2] = 160 + texture;
	});
	FakeNetworks networks;
	// a face of 160 x 200 with its centre at (324, 244)
	networks.outputs.Put(0, 40, 30, 160, 200, 0.9f);
	// a second, weaker and far too small one
	networks.outputs.Put(0, 10, 10, 12, 12, 0.7f);

	std::vector<DetectedFace> faces;
	std::vector<FaceCandidate> pixels;
	FaceScanOptions options;
	// the fixture's scores, whatever the application's threshold is
	options.minScore = 0.6f;
	CHECK(FindFaces(networks, picture.bits.data(), 640, 480, 2560, options,
		faces, &pixels));
	CHECK_EQ(networks.detectorRuns, 1);
	CHECK_EQ(faces.size(), (size_t)2);
	if (faces.size() != 2)
		return;
	CHECK_EQ(pixels.size(), (size_t)2);

	const DetectedFace& face = faces[0];
	CHECK(near(face.x, (324 - 80) / 640.0f, 1e-4f));
	CHECK(near(face.y, (244 - 100) / 480.0f, 1e-4f));
	CHECK(near(face.width, 160 / 640.0f, 1e-4f));
	CHECK(near(face.height, 200 / 480.0f, 1e-4f));
	CHECK(near(face.score, 0.9f, 1e-5f));
	CHECK(face.quality >= kFaceMinQuality);
	CHECK(face.landmarks[0][0] < face.landmarks[1][0]);
	CHECK(face.landmarks[2][1] > face.landmarks[0][1]);
	CHECK_EQ(face.embedding.size(), (size_t)4);
	// normalised, and in the picture's colours (blue : green : red)
	float length = 0;
	for (float value : face.embedding)
		length += value * value;
	CHECK(near(length, 1, 1e-4f));
	CHECK(near(face.embedding[1] / face.embedding[0], 96.0f / 56, 0.05f));
	CHECK(near(face.embedding[2] / face.embedding[0], 176.0f / 56, 0.1f));

	// the small one is reported without an embedding
	CHECK(faces[1].quality < kFaceMinQuality);
	CHECK(faces[1].embedding.empty());
	CHECK_EQ(networks.embedderRuns, 1);

	// without embeddings the embedder does not run
	options.embed = false;
	CHECK(FindFaces(networks, picture.bits.data(), 640, 480, 2560, options,
		faces));
	CHECK_EQ(networks.embedderRuns, 1);
	CHECK(faces[0].embedding.empty());
	CHECK(faces[0].quality >= kFaceMinQuality);

	// a higher bar for the score
	options.minScore = 0.8f;
	CHECK(FindFaces(networks, picture.bits.data(), 640, 480, 2560, options,
		faces));
	CHECK_EQ(faces.size(), (size_t)1);

	// a network that fails fails the search
	options = FaceScanOptions();
	options.minScore = 0.6f;
	networks.failEmbedder = true;
	CHECK(!FindFaces(networks, picture.bits.data(), 640, 480, 2560, options,
		faces));
	networks.failEmbedder = false;
	networks.failDetector = true;
	CHECK(!FindFaces(networks, picture.bits.data(), 640, 480, 2560, options,
		faces));
	networks.failDetector = false;

	// nonsense in
	CHECK(!FindFaces(networks, nullptr, 640, 480, 2560, options, faces));
	CHECK(!FindFaces(networks, picture.bits.data(), 640, 480, 100, options,
		faces));
	CHECK(!FindFaces(networks, picture.bits.data(), 0, 480, 2560, options,
		faces));
}


TEST(FaceFindingSearchesTiles)
{
	Picture picture(2000, 1500, [](int32_t x, int32_t y, uint8_t* pixel) {
		pixel[0] = pixel[1] = pixel[2] = 100 + ((x + y) % 4) * 10;
	});
	FakeNetworks networks;
	// the same answer for the whole picture and each of the four tiles:
	// five faces of different sizes and places, none of which overlap
	networks.outputs.Put(1, 20, 20, 60, 60, 0.9f);
	std::vector<DetectedFace> faces;
	FaceScanOptions options;
	options.tileScale = 0.5f;
	options.embed = false;
	CHECK(FindFaces(networks, picture.bits.data(), 2000, 1500, 8000, options,
		faces));
	CHECK_EQ(networks.detectorRuns, 5);
	CHECK_EQ(faces.size(), (size_t)5);
	for (const DetectedFace& face : faces) {
		CHECK(face.x >= 0 && face.y >= 0);
		CHECK(face.x + face.width <= 1.0001f);
		CHECK(face.y + face.height <= 1.0001f);
	}
	// the whole picture's is the large one: 60 input pixels at scale 0.32
	bool large = false;
	for (const DetectedFace& face : faces)
		large = large || near(face.width * 2000, 60 / 0.32f, 0.5f);
	CHECK(large);
}
