// facescan_host: the face pipeline of Photos (src/core/FaceMath) on the
// build host, with the networks run by the Neural Kit's CPU interpreter as
// the kit's own host tests build it (tools/face-test/build-host.sh). This is
// what docs/FACES.md's numbers were measured with: the code is the
// application's, only the pictures are read as PPM instead of through the
// Translation Kit.
//
//   facescan_host --models DIR [options] PICTURE.ppm... | --list FILE
//
//   --detectors N,N       input sizes of the detectors (default 640,1280)
//   --tile-scale F        as FaceScanOptions (0: no tiles)
//   --tile-overlap F
//   --min-score F, --min-quality F
//   --no-embed
//   --dump DIR            the aligned crops as PPM
//
// Prints one line of JSON per picture.

#include "core/FaceMath.h"

#include <NeuralGraphData.h>
#include <NeuralInterpreter.h>

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace BPrivate;
using namespace photos;


static double
now_ms()
{
	timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec * 1e3 + time.tv_nsec / 1e6;
}


class HostModel {
public:
	HostModel()
		:
		fGraph(NULL),
		fInterpreter(NULL)
	{
	}

	~HostModel()
	{
		delete fInterpreter;
		for (size_t i = 0; i < fTensors.size(); i++) {
			if (fOwned[i])
				free(fTensors[i]);
		}
		if (fGraph != NULL)
			fGraph->ReleaseReference();
	}

	bool Load(const std::string& path)
	{
		int fd = open(path.c_str(), O_RDONLY);
		struct stat stat;
		if (fd < 0 || fstat(fd, &stat) != 0) {
			if (fd >= 0)
				close(fd);
			return false;
		}
		void* file = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (file == MAP_FAILED)
			return false;
		fGraph = new NeuralGraphData();
		fGraph->AdoptBuffer(file, stat.st_size, true);
		if (neural_load_tflite(file, stat.st_size, fGraph) != B_OK) {
			fprintf(stderr, "%s: not loaded\n", path.c_str());
			return false;
		}
		int32 count = fGraph->CountOperators();
		std::vector<int32> order(count);
		for (int32 i = 0; i < count; i++)
			order[i] = i;
		fInterpreter = new NeuralInterpreter(fGraph);
		if (fInterpreter->Prepare(order.data(), count) != B_OK) {
			fprintf(stderr, "%s: not prepared\n", path.c_str());
			return false;
		}
		for (int32 i = 0; i < fGraph->CountTensors(); i++) {
			const neural_graph_tensor& tensor = fGraph->Tensor(i);
			void* memory = (void*)tensor.data;
			bool owned = memory == NULL;
			if (owned && posix_memalign(&memory, 64, tensor.size) != 0)
				return false;
			if (owned)
				memset(memory, 0, tensor.size);
			fTensors.push_back(memory);
			fOwned.push_back(owned);
		}
		return true;
	}

	float* Input() { return (float*)fTensors[fGraph->InputAt(0)]; }
	int32 InputSize() const
	{
		return fGraph->Tensor(fGraph->InputAt(0)).dimensions[1];
	}

	const float* Output(const char* name) const
	{
		for (int32 i = 0; i < fGraph->CountOutputs(); i++) {
			const neural_graph_tensor& tensor
				= fGraph->Tensor(fGraph->OutputAt(i));
			if (name == NULL || (tensor.name != NULL
					&& strcmp(tensor.name, name) == 0)) {
				return (const float*)fTensors[fGraph->OutputAt(i)];
			}
		}
		return NULL;
	}

	int32 OutputCount() const
	{
		const neural_graph_tensor& tensor
			= fGraph->Tensor(fGraph->OutputAt(0));
		return tensor.dimensions[tensor.rank - 1];
	}

	bool Run() { return fInterpreter->Run(fTensors.data()) == B_OK; }

private:
	NeuralGraphData*	fGraph;
	NeuralInterpreter*	fInterpreter;
	std::vector<void*>	fTensors;
	std::vector<bool>	fOwned;
};


class HostNetworks : public FaceNetworks {
public:
	std::vector<HostModel*> detectors;
	HostModel embedder;
	double detectTime = 0, embedTime = 0;
	int detectRuns = 0, embedRuns = 0;

	virtual int32_t CountDetectors() const { return detectors.size(); }
	virtual int32_t DetectorInputSize(int32_t index) const
	{
		return detectors[index]->InputSize();
	}
	virtual float* DetectorInput(int32_t index)
	{
		return detectors[index]->Input();
	}
	virtual bool RunDetector(int32_t index, FaceDetectorOutputs& outputs)
	{
		double start = now_ms();
		if (!detectors[index]->Run())
			return false;
		detectTime += now_ms() - start;
		detectRuns++;
		for (int32_t i = 0; i < kFaceStrideCount; i++) {
			char name[32];
			snprintf(name, sizeof(name), "cls_%d", (int)kFaceStrides[i]);
			outputs.cls[i] = detectors[index]->Output(name);
			snprintf(name, sizeof(name), "obj_%d", (int)kFaceStrides[i]);
			outputs.obj[i] = detectors[index]->Output(name);
			snprintf(name, sizeof(name), "bbox_%d", (int)kFaceStrides[i]);
			outputs.bbox[i] = detectors[index]->Output(name);
			snprintf(name, sizeof(name), "kps_%d", (int)kFaceStrides[i]);
			outputs.kps[i] = detectors[index]->Output(name);
		}
		return true;
	}
	virtual int32_t EmbeddingSize() const
	{
		return const_cast<HostModel&>(embedder).OutputCount();
	}
	virtual float* EmbedderInput() { return embedder.Input(); }
	virtual const float* RunEmbedder()
	{
		double start = now_ms();
		if (!embedder.Run())
			return NULL;
		embedTime += now_ms() - start;
		embedRuns++;
		return embedder.Output(NULL);
	}
};


// A binary PPM as a B_RGB32-layout buffer.
static bool
read_ppm(const char* path, std::vector<uint8_t>& bits, int& width, int& height)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	int maximum = 0;
	bool ok = fscanf(file, "P6 %d %d %d", &width, &height, &maximum) == 3
		&& maximum == 255 && width > 0 && height > 0 && fgetc(file) != EOF;
	if (ok) {
		std::vector<uint8_t> rgb((size_t)width * height * 3);
		ok = fread(rgb.data(), 1, rgb.size(), file) == rgb.size();
		bits.resize((size_t)width * height * 4);
		for (size_t i = 0; ok && i < (size_t)width * height; i++) {
			bits[i * 4] = rgb[i * 3 + 2];
			bits[i * 4 + 1] = rgb[i * 3 + 1];
			bits[i * 4 + 2] = rgb[i * 3];
			bits[i * 4 + 3] = 255;
		}
	}
	fclose(file);
	return ok;
}


static void
write_crop(const std::string& path, const float* tensor)
{
	std::vector<uint8_t> bgr(kFaceCropSize * kFaceCropSize * 3);
	FaceCropToBytes(tensor, bgr.data());
	for (size_t i = 0; i < bgr.size(); i += 3)
		std::swap(bgr[i], bgr[i + 2]);
	FILE* file = fopen(path.c_str(), "wb");
	if (file == NULL)
		return;
	fprintf(file, "P6 %d %d 255\n", (int)kFaceCropSize, (int)kFaceCropSize);
	fwrite(bgr.data(), 1, bgr.size(), file);
	fclose(file);
}


int
main(int argc, char** argv)
{
	std::string models = "models";
	std::string dump;
	std::vector<std::string> pictures;
	FaceScanOptions options = FaceScanOptions::Default();
	std::vector<int> sizes;
	sizes.push_back(640);
	sizes.push_back(1280);
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		bool more = i + 1 < argc;
		if (arg == "--models" && more)
			models = argv[++i];
		else if (arg == "--tile-scale" && more)
			options.tileScale = atof(argv[++i]);
		else if (arg == "--tile-overlap" && more)
			options.tileOverlap = atof(argv[++i]);
		else if (arg == "--detectors" && more) {
			sizes.clear();
			for (char* next = strtok(argv[++i], ","); next != NULL;
					next = strtok(NULL, ",")) {
				sizes.push_back(atoi(next));
			}
		}
		else if (arg == "--min-score" && more)
			options.minScore = atof(argv[++i]);
		else if (arg == "--min-quality" && more)
			options.minQuality = atof(argv[++i]);
		else if (arg == "--no-embed")
			options.embed = false;
		else if (arg == "--dump" && more)
			dump = argv[++i];
		else if (arg == "--list" && more) {
			FILE* list = fopen(argv[++i], "r");
			char line[4096];
			while (list != NULL && fgets(line, sizeof(line), list) != NULL) {
				line[strcspn(line, "\r\n")] = '\0';
				if (line[0] != '\0')
					pictures.push_back(line);
			}
			if (list != NULL)
				fclose(list);
		} else if (arg[0] == '-') {
			fprintf(stderr, "usage: %s --models DIR [--detectors N,N] "
				"[--tile-scale F] [--tile-overlap F] "
				"[--min-score F] [--min-quality F] [--no-embed] [--dump DIR] "
				"PICTURE.ppm... | --list FILE\n", argv[0]);
			return 2;
		} else
			pictures.push_back(arg);
	}

	HostNetworks networks;
	for (int size : sizes) {
		HostModel* detector = new HostModel();
		char name[64];
		snprintf(name, sizeof(name), "/face_detector_%d.tflite", size);
		if (!detector->Load(models + name))
			return 1;
		networks.detectors.push_back(detector);
	}
	if (!networks.embedder.Load(models + "/face_embedder.tflite"))
		return 1;

	for (const std::string& picture : pictures) {
		std::vector<uint8_t> bits;
		int width, height;
		if (!read_ppm(picture.c_str(), bits, width, height)) {
			fprintf(stderr, "%s: not a binary PPM\n", picture.c_str());
			continue;
		}
		std::vector<DetectedFace> faces;
		std::vector<FaceCandidate> pixels;
		networks.detectTime = networks.embedTime = 0;
		networks.detectRuns = networks.embedRuns = 0;
		double start = now_ms();
		if (!FindFaces(networks, bits.data(), width, height, width * 4,
				options, faces, &pixels)) {
			fprintf(stderr, "%s: failed\n", picture.c_str());
			continue;
		}
		double total = now_ms() - start;
		printf("{\"image\": \"%s\", \"width\": %d, \"height\": %d, "
			"\"ms\": %.1f, \"detect_ms\": %.1f, \"detect_runs\": %d, "
			"\"embed_ms\": %.1f, \"embed_runs\": %d, \"faces\": [",
			picture.c_str(), width, height, total, networks.detectTime,
			networks.detectRuns, networks.embedTime, networks.embedRuns);
		for (size_t i = 0; i < faces.size(); i++) {
			const FaceCandidate& face = pixels[i];
			std::vector<float> crop(kFaceCropSize * kFaceCropSize * 3);
			FaceTransform transform;
			float sharpness = 0;
			if (AlignFace(face.landmarks, transform)) {
				WarpFace(bits.data(), width, height, width * 4, transform,
					crop.data());
				sharpness = FaceSharpness(crop.data());
				if (!dump.empty()) {
					std::string base = picture.substr(
						picture.find_last_of('/') + 1);
					char name[64];
					snprintf(name, sizeof(name), "-%zu.ppm", i);
					write_crop(dump + "/" + base + name, crop.data());
				}
			}
			float eyeX = face.landmarks[1][0] - face.landmarks[0][0];
			float eyeY = face.landmarks[1][1] - face.landmarks[0][1];
			printf("%s{\"box\": [%.1f, %.1f, %.1f, %.1f], \"score\": %.4f, "
				"\"quality\": %.4f, \"sharp\": %.4f, \"frontal\": %.4f, "
				"\"eyes\": %.2f, \"lm\": [", i > 0 ? ", " : "", face.x0,
				face.y0, face.x1, face.y1, face.score, faces[i].quality,
				sharpness, FaceFrontalness(face.landmarks),
				sqrtf(eyeX * eyeX + eyeY * eyeY));
			for (int n = 0; n < kFaceLandmarkCount; n++) {
				printf("%s%.1f, %.1f", n > 0 ? ", " : "", face.landmarks[n][0],
					face.landmarks[n][1]);
			}
			printf("], \"emb\": [");
			for (size_t n = 0; n < faces[i].embedding.size(); n++)
				printf("%s%.5f", n > 0 ? "," : "", faces[i].embedding[n]);
			printf("]}");
		}
		printf("]}\n");
		fflush(stdout);
	}
	return 0;
}
