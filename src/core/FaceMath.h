// Faces without a neural network runtime: what goes into the detector and
// the embedder and what is made of their outputs. The networks themselves
// are run by someone else behind FaceNetworks (platform/FaceEngine with the
// Neural Kit; the tests with made-up outputs), so all of this is tested on
// the build host. docs/FACES.md has the models and where the numbers below
// come from.
#pragma once

#include <cstdint>
#include <vector>

namespace photos {

// A detection below this score is not a face.
extern const float kFaceMinScore;
// Faces below this quality are reported (box, landmarks) but get no
// embedding: too small, turned away, blurred or doubtful to tell who it is.
extern const float kFaceMinQuality;

// Embeddings are compared by cosine similarity (CosineSimilarity(); of two
// DetectedFace embeddings, which are normalised, the dot product).
//
// At or above kSameFaceThreshold two faces are the same person, except that
// brothers and sisters as babies can be this close: safe as the link when
// faces nobody has named are grouped, and for assigning a face to a person
// when no other person comes near (see ClassifyFaceMatch()).
extern const float kSameFaceThreshold;
// How much closer than to anybody else a face must be to a person for that.
extern const float kSameFaceMargin;
// At or above this the same person whoever else is near.
extern const float kCertainFaceThreshold;
// At or above this they may well be the same: worth asking ("Is this ...?").
// Too low for grouping by links: different people are this close now and
// then, and chains of such links would join them.
extern const float kMaybeSameFaceThreshold;

enum FaceMatch {
	kFaceNoMatch,		// nobody we know
	kFaceMaybeMatch,	// ask
	kFaceSameMatch		// assign without asking
};

// What to make of a face from its highest similarity to a face of the most
// similar person (`best`) and its highest similarity to a face of any other
// person (`nextBest`; below zero when there is nobody else).
FaceMatch ClassifyFaceMatch(float best, float nextBest);

const int32_t kFaceLandmarkCount = 5;
// The embedder's input: a square of this many pixels, three channels.
const int32_t kFaceCropSize = 112;
// The detector's three grids.
const int32_t kFaceStrideCount = 3;
extern const int32_t kFaceStrides[kFaceStrideCount];

// What FaceEngine::Detect() returns for one face.
struct DetectedFace {
	float x, y, width, height;	// box as fractions of the picture
	float score;				// detector confidence, 0..1
	float quality;				// 0..1: FaceQuality()
	// Fractions of the picture: right eye, left eye, nose tip, right and
	// left corner of the mouth, as the subject has them (the "right eye" is
	// the one on the left of an upright picture).
	float landmarks[kFaceLandmarkCount][2];
	// L2-normalised; empty when quality < kFaceMinQuality.
	std::vector<float> embedding;
};

// A face in pixels of the picture, between detection and the result.
struct FaceCandidate {
	float x0, y0, x1, y1;
	float score;
	float landmarks[kFaceLandmarkCount][2];
};

// A part of a picture that goes through the detector at once: scaled by
// `scale` into the top left of the square input, the rest of which is black.
struct FaceWindow {
	int32_t left, top, width, height;	// in pixels of the picture
	float scale;						// input pixels per picture pixel, <= 1
	int32_t detector;					// which of the FaceNetworks' detectors
	bool whole;							// the whole picture (not a tile)
};

struct FaceScanOptions {
	// A picture goes through every detector whole (scaled to fit its
	// input), the smallest input first; a larger one is only used when it
	// shows the picture noticeably larger. Pictures that are still much
	// reduced in the largest input are searched in overlapping tiles of it
	// at this scale as well (input pixels per picture pixel; 0: no tiles).
	float tileScale = 0.5f;
	// How much of a tile its neighbour covers again (fraction of the tile).
	float tileOverlap = 0.25f;
	bool embed = true;
	float minScore = -1;		// < 0: kFaceMinScore
	float minQuality = -1;		// < 0: kFaceMinQuality
	int32_t maxFaces = 200;

	// What Photos scans with.
	static FaceScanOptions Default();
};

// The detector's outputs, per stride (8, 16, 32): cells in row order.
struct FaceDetectorOutputs {
	const float* cls[kFaceStrideCount];		// [cells]
	const float* obj[kFaceStrideCount];		// [cells]
	const float* bbox[kFaceStrideCount];	// [cells, 4]
	const float* kps[kFaceStrideCount];		// [cells, 10]
};

// The windows a picture is searched in: the whole picture first. The
// detectors' input sizes are in rising order.
void PlanFaceWindows(int32_t width, int32_t height, const int32_t* inputSizes,
	int32_t detectorCount, const FaceScanOptions& options,
	std::vector<FaceWindow>& windows);

// Fills the detector's input ([inputSize, inputSize, 3] floats: blue, green,
// red, 0..255) with a window of a B_RGB32-layout picture (bytes blue, green,
// red, alpha), averaging the pixels that fall into one.
void FillDetectorInput(const uint8_t* bits, int32_t width, int32_t height,
	int32_t bytesPerRow, const FaceWindow& window, int32_t inputSize,
	float* tensor);

// Adds the faces the detector found in a window, in pixels of the picture.
// Faces of a tile that touch an edge of it which is not an edge of the
// picture are left out: they are cut, and a neighbour or the whole picture
// has them complete.
void DecodeFaces(const FaceDetectorOutputs& outputs, int32_t inputSize,
	const FaceWindow& window, int32_t pictureWidth, int32_t pictureHeight,
	float minScore, std::vector<FaceCandidate>& candidates);

// Best first; drops what overlaps a better one (intersection over union
// above maxOverlap, or mostly inside it).
void SuppressOverlaps(std::vector<FaceCandidate>& candidates,
	float maxOverlap = 0.3f, int32_t maxFaces = 200);

// crop = [a -b; b a] * picture + (tx, ty): rotation, uniform scale, shift.
struct FaceTransform {
	float a, b, tx, ty;

	float Scale() const;		// crop pixels per picture pixel
};

// Where the five landmarks lie in the embedder's crop.
extern const float kFaceTemplate[kFaceLandmarkCount][2];

// The similarity transform that brings the landmarks (pixels of the
// picture) closest to the template (least squares). False when the
// landmarks are degenerate.
bool AlignFace(const float landmarks[kFaceLandmarkCount][2],
	FaceTransform& transform);

// The aligned face as the embedder takes it: [112, 112, 3] floats, (blue,
// green, red - 127.5) / 128. Outside the picture is black.
void WarpFace(const uint8_t* bits, int32_t width, int32_t height,
	int32_t bytesPerRow, const FaceTransform& transform, float* tensor);

// The tensor WarpFace() made as bytes again (blue, green, red per pixel):
// for looking at.
void FaceCropToBytes(const float* tensor, uint8_t* bgr);

// How much detail an aligned crop has, 0..1 (blurred faces are low).
float FaceSharpness(const float* tensor);

// How much the face looks into the camera, 0 (profile) .. 1, from the
// landmarks.
float FaceFrontalness(const float landmarks[kFaceLandmarkCount][2]);

// One number for "can this face be told from others": the distance of its
// eyes in pixels, how frontal it is, how sharp the crop.
float FaceQuality(const FaceCandidate& face, float sharpness);

void NormalizeL2(float* values, int32_t count);
// Of two vectors of any length; of normalised ones it is their dot product.
float CosineSimilarity(const float* a, const float* b, int32_t count);

// The two networks, run by someone else.
class FaceNetworks {
public:
	virtual ~FaceNetworks() {}

	// The same network for inputs of several sizes, the smallest first.
	virtual int32_t CountDetectors() const = 0;
	virtual int32_t DetectorInputSize(int32_t detector) const = 0;
	// [size, size, 3] floats to fill before RunDetector().
	virtual float* DetectorInput(int32_t detector) = 0;
	virtual bool RunDetector(int32_t detector,
		FaceDetectorOutputs& outputs) = 0;

	virtual int32_t EmbeddingSize() const = 0;
	// [112, 112, 3] floats to fill before RunEmbedder().
	virtual float* EmbedderInput() = 0;
	// EmbeddingSize() numbers (not normalised), or NULL.
	virtual const float* RunEmbedder() = 0;
};

// The whole of it: searches a B_RGB32-layout picture, aligns, rates and
// embeds what it finds. Best faces first. False when a network failed.
bool FindFaces(FaceNetworks& networks, const uint8_t* bits, int32_t width,
	int32_t height, int32_t bytesPerRow, const FaceScanOptions& options,
	std::vector<DetectedFace>& faces, std::vector<FaceCandidate>* pixels = nullptr);

}	// namespace photos
