#include "FaceEngine.h"

#include <Autolock.h>
#include <Locker.h>
#include <NeuralKit.h>
#include <OS.h>
#include <String.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include <unistd.h>

namespace photos {

// 1: YuNet 2023mar, SFace 2021dec, FaceMath as of docs/FACES.md.
const int32 FaceEngine::kModelVersion = 1;

namespace {

// How far a device's outputs may be from the CPU's on the test picture,
// relative to each output's largest value.
const float kDeviceTolerance = 2e-3f;


// Something like a picture (smooth, with texture), the same every time.
void
fill_test_picture(BTensor* tensor, float low, float high)
{
	float* values = (float*)tensor->Data();
	int64 count = tensor->CountElements();
	int32 width = tensor->Rank() == 4 ? tensor->DimensionAt(2) : 1;
	uint32 state = 12345;
	for (int64 i = 0; i < count; i++) {
		int64 pixel = i / 3;
		int32 x = pixel % width;
		int32 y = pixel / width;
		state = state * 1664525 + 1013904223;
		float value = 0.5f + 0.3f * sinf(x * 0.11f + (i % 3)) * cosf(y * 0.07f)
			+ 0.2f * ((state >> 24) / 255.0f - 0.5f);
		value = value < 0 ? 0 : (value > 1 ? 1 : value);
		values[i] = low + value * (high - low);
	}
}


// Whether two sessions of one model give the same outputs for the same
// input.
bool
sessions_agree(BNeuralSession& session, BNeuralSession& reference, float low,
	float high)
{
	fill_test_picture(session.InputAt(0), low, high);
	if (reference.InputAt(0)->CopyFrom(*session.InputAt(0)) != B_OK)
		return false;
	if (session.Run() != B_OK || reference.Run() != B_OK)
		return false;
	for (int32 i = 0; i < reference.CountOutputs(); i++) {
		const BTensor* mine = session.OutputAt(i);
		const BTensor* theirs = reference.OutputAt(i);
		if (mine == NULL || theirs == NULL
			|| mine->CountElements() != theirs->CountElements()) {
			return false;
		}
		const float* a = (const float*)mine->Data();
		const float* b = (const float*)theirs->Data();
		float largest = 1e-6f, difference = 0;
		for (int64 j = 0; j < theirs->CountElements(); j++) {
			if (!std::isfinite(a[j]))
				return false;
			largest = std::max(largest, fabsf(b[j]));
			difference = std::max(difference, fabsf(a[j] - b[j]));
		}
		if (difference > kDeviceTolerance * largest)
			return false;
	}
	return true;
}


// One network on a device, or on the CPU when the device cannot be
// trusted with it.
struct Network {
	BNeuralModel	model;
	BNeuralSession*	session;

	Network()
		:
		session(NULL)
	{
	}

	~Network()
	{
		delete session;
	}

	status_t Load(const char* directory, const char* name, uint32 flags,
		float low, float high)
	{
		BString path(directory);
		path << "/" << name;
		status_t status = model.SetTo(path.String());
		if (status != B_OK) {
			fprintf(stderr, "FaceEngine: %s: %s %s\n", path.String(),
				neural_error_string(status), model.ErrorDetails());
			return status;
		}
		BTensorInfo info;
		if (model.CountInputs() != 1 || model.GetInputInfo(0, &info) != B_OK
			|| info.Type() != B_NEURAL_FLOAT32 || info.Rank() != 4
			|| info.DimensionAt(1) != info.DimensionAt(2)
			|| info.DimensionAt(3) != 3) {
			return B_NEURAL_SHAPE_MISMATCH;
		}

		session = new BNeuralSession(model, flags);
		status = session->InitCheck();
		if (status != B_OK && (flags & B_NEURAL_CPU_ONLY) == 0) {
			delete session;
			session = new BNeuralSession(model, B_NEURAL_CPU_ONLY);
			status = session->InitCheck();
		}
		if (status != B_OK)
			return status;

		if (session->Device().Kind() != B_NEURAL_DEVICE_CPU) {
			// Does the device compute what the CPU does? A driver that
			// does not would fill the library with wrong faces quietly.
			BNeuralSession* reference = new BNeuralSession(model,
				B_NEURAL_CPU_ONLY);
			if (reference->InitCheck() != B_OK) {
				delete reference;
				return B_OK;
			}
			if (sessions_agree(*session, *reference, low, high)) {
				delete reference;
			} else {
				fprintf(stderr, "FaceEngine: %s on %s does not agree with "
					"the CPU: using the CPU\n", name,
					session->Device().Name());
				delete session;
				session = reference;
			}
		}
		return B_OK;
	}

	int32 InputSize() const
	{
		return session->InputAt(0)->DimensionAt(1);
	}
};

}	// namespace


class FaceEngine::Networks : public FaceNetworks {
public:
	// the same network for inputs of rising size
	std::vector<Network*> detectors;
	Network			embedder;
	BLocker			lock;
	BString			deviceName;
	bool			ready;
	bigtime_t		detectorTime;
	bigtime_t		embedderTime;
	int32			detectorRuns;
	int32			embedderRuns;

	Networks()
		:
		lock("face engine"),
		ready(false),
		detectorTime(0),
		embedderTime(0),
		detectorRuns(0),
		embedderRuns(0)
	{
	}

	virtual ~Networks()
	{
		Unload();
	}

	void Unload()
	{
		for (Network* detector : detectors)
			delete detector;
		detectors.clear();
		delete embedder.session;
		embedder.session = NULL;
		ready = false;
	}

	virtual int32_t CountDetectors() const
	{
		return detectors.size();
	}

	virtual int32_t DetectorInputSize(int32_t index) const
	{
		return detectors[index]->InputSize();
	}

	virtual float* DetectorInput(int32_t index)
	{
		return (float*)detectors[index]->session->InputAt(0)->Data();
	}

	virtual bool RunDetector(int32_t index, FaceDetectorOutputs& outputs)
	{
		BNeuralSession* session = detectors[index]->session;
		bigtime_t start = system_time();
		if (session->Run() != B_OK)
			return false;
		detectorTime += system_time() - start;
		detectorRuns++;

		int32 size = detectors[index]->InputSize();
		for (int32 i = 0; i < kFaceStrideCount; i++) {
			int64 cells = (int64)(size / kFaceStrides[i])
				* (size / kFaceStrides[i]);
			outputs.cls[i] = _Output(session, "cls", i, cells);
			outputs.obj[i] = _Output(session, "obj", i, cells);
			outputs.bbox[i] = _Output(session, "bbox", i, cells * 4);
			outputs.kps[i] = _Output(session, "kps", i, cells * 10);
			if (outputs.cls[i] == NULL || outputs.obj[i] == NULL
				|| outputs.bbox[i] == NULL || outputs.kps[i] == NULL) {
				return false;
			}
		}
		return true;
	}

	virtual int32_t EmbeddingSize() const
	{
		const BTensor* output = embedder.session != NULL
			? embedder.session->OutputAt(0) : NULL;
		return output != NULL ? (int32_t)output->CountElements() : 0;
	}

	virtual float* EmbedderInput()
	{
		return (float*)embedder.session->InputAt(0)->Data();
	}

	virtual const float* RunEmbedder()
	{
		bigtime_t start = system_time();
		if (embedder.session->Run() != B_OK)
			return NULL;
		embedderTime += system_time() - start;
		embedderRuns++;
		return (const float*)embedder.session->OutputAt(0)->Data();
	}

private:
	static const float* _Output(const BNeuralSession* session,
		const char* kind, int32 level, int64 count)
	{
		char name[32];
		snprintf(name, sizeof(name), "%s_%d", kind, (int)kFaceStrides[level]);
		const BTensor* tensor = session->FindOutput(name);
		if (tensor == NULL || tensor->Type() != B_NEURAL_FLOAT32
			|| tensor->CountElements() != count) {
			return NULL;
		}
		return (const float*)tensor->Data();
	}
};


FaceEngine::FaceEngine()
	:
	fNetworks(new Networks())
{
	memset(&fTiming, 0, sizeof(fTiming));
}


FaceEngine::~FaceEngine()
{
	delete fNetworks;
}


status_t
FaceEngine::Init(const char* modelDirectory, uint32 flags)
{
	BAutolock locker(fNetworks->lock);
	if (fNetworks->ready)
		return B_OK;
	if (modelDirectory == NULL)
		return B_BAD_VALUE;

	// The detector takes 0..255, the embedder (x - 127.5) / 128. The small
	// detector is needed, the large one (people at the back) is used when
	// it is there.
	static const int32 kDetectorSizes[] = { 640, 1280 };
	status_t status = B_OK;
	for (size_t i = 0; i < B_COUNT_OF(kDetectorSizes) && status == B_OK; i++) {
		BString name;
		name.SetToFormat("face_detector_%d.tflite", (int)kDetectorSizes[i]);
		BString path(modelDirectory);
		path << "/" << name;
		if (i > 0 && access(path.String(), R_OK) != 0)
			continue;
		Network* detector = new Network();
		status_t loaded = detector->Load(modelDirectory, name.String(), flags,
			0, 255);
		if (loaded == B_OK && (detector->InputSize() != kDetectorSizes[i]
				|| detector->InputSize() % 32 != 0)) {
			loaded = B_NEURAL_SHAPE_MISMATCH;
		}
		if (loaded != B_OK) {
			delete detector;
			if (i == 0)
				status = loaded;
			continue;
		}
		fNetworks->detectors.push_back(detector);
	}
	if (status == B_OK) {
		status = fNetworks->embedder.Load(modelDirectory,
			"face_embedder.tflite", flags, -1, 1);
	}
	if (status == B_OK) {
		BTensor* crop = fNetworks->embedder.session->InputAt(0);
		if (crop->DimensionAt(1) != kFaceCropSize
			|| fNetworks->EmbeddingSize() <= 0) {
			status = B_NEURAL_SHAPE_MISMATCH;
		}
	}
	if (status != B_OK) {
		fNetworks->Unload();
		return status;
	}

	// The embedder is where the time goes: its device is the one to name.
	const BNeuralDevice& device = fNetworks->embedder.session->Device();
	if (device.Kind() == B_NEURAL_DEVICE_CPU)
		fNetworks->deviceName = "CPU";
	else {
		fNetworks->deviceName = device.Name();
		fNetworks->deviceName << " ("
			<< (device.Kind() == B_NEURAL_DEVICE_GPU ? "GPU" : "NPU") << ")";
	}
	fNetworks->ready = true;
	return B_OK;
}


bool
FaceEngine::IsReady() const
{
	return fNetworks->ready;
}


status_t
FaceEngine::Detect(const uint8* bits, int32 width, int32 height,
	int32 bytesPerRow, std::vector<DetectedFace>& faces)
{
	return Detect(bits, width, height, bytesPerRow,
		FaceScanOptions::Default(), faces, NULL);
}


status_t
FaceEngine::Detect(const uint8* bits, int32 width, int32 height,
	int32 bytesPerRow, const FaceScanOptions& options,
	std::vector<DetectedFace>& faces, std::vector<FaceCandidate>* pixels)
{
	faces.clear();
	BAutolock locker(fNetworks->lock);
	if (!fNetworks->ready)
		return B_NO_INIT;
	if (bits == NULL || width <= 0 || height <= 0 || bytesPerRow < width * 4)
		return B_BAD_VALUE;

	fNetworks->detectorTime = fNetworks->embedderTime = 0;
	fNetworks->detectorRuns = fNetworks->embedderRuns = 0;
	bigtime_t start = system_time();
	bool ok = FindFaces(*fNetworks, bits, width, height, bytesPerRow, options,
		faces, pixels);
	fTiming.total = system_time() - start;
	fTiming.detector = fNetworks->detectorTime;
	fTiming.embedder = fNetworks->embedderTime;
	fTiming.detectorRuns = fNetworks->detectorRuns;
	fTiming.embedderRuns = fNetworks->embedderRuns;
	if (!ok) {
		faces.clear();
		return B_ERROR;
	}
	return B_OK;
}


int32
FaceEngine::EmbeddingSize() const
{
	return fNetworks->ready ? fNetworks->EmbeddingSize() : 0;
}


const char*
FaceEngine::DeviceName() const
{
	return fNetworks->deviceName.String();
}


const FaceEngine::Timing&
FaceEngine::LastTiming() const
{
	return fTiming;
}

}	// namespace photos
