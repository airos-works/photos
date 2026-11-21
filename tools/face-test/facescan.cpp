// facescan: the face engine of Photos from a shell, on air/OS.
//
//   facescan [options] PICTURE...
//
//   --models DIR      where the .tflite files are (default: "models" next
//                     to the program, then ./models)
//   --cpu             the CPU, whatever else there is
//   --low-power       B_NEURAL_PREFER_LOW_POWER
//   --tile-scale F    as FaceScanOptions (0: the whole picture only)
//   --min-score F, --min-quality F
//   --no-embed
//   --compare         the similarity of every face to every other, over
//                     all pictures given (same person at 0.4 .. 1)
//   --dump DIR        the aligned crops as PPM files
//   --bench [N]       every picture N times (default 10): the best time
//   --json            one line of JSON per picture, as facescan_host's
//
// Pictures are read through the Translation Kit (any format it knows); a
// binary PPM is read as it is.

#include "platform/FaceEngine.h"

#include <Application.h>
#include <Bitmap.h>
#include <NeuralKit.h>
#include <Path.h>
#include <String.h>
#include <TranslationUtils.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace photos;

namespace {

struct Picture {
	int32 width, height, bytesPerRow;
	std::vector<uint8> bits;
};


bool
read_ppm(const char* path, Picture& picture)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	int width = 0, height = 0, maximum = 0;
	bool ok = fscanf(file, "P6 %d %d %d", &width, &height, &maximum) == 3
		&& maximum == 255 && width > 0 && height > 0 && fgetc(file) != EOF;
	if (ok) {
		std::vector<uint8> rgb((size_t)width * height * 3);
		ok = fread(rgb.data(), 1, rgb.size(), file) == rgb.size();
		picture.width = width;
		picture.height = height;
		picture.bytesPerRow = width * 4;
		picture.bits.resize((size_t)width * height * 4);
		for (size_t i = 0; ok && i < (size_t)width * height; i++) {
			picture.bits[i * 4] = rgb[i * 3 + 2];
			picture.bits[i * 4 + 1] = rgb[i * 3 + 1];
			picture.bits[i * 4 + 2] = rgb[i * 3];
			picture.bits[i * 4 + 3] = 255;
		}
	}
	fclose(file);
	return ok;
}


bool
read_picture(const char* path, Picture& picture)
{
	size_t length = strlen(path);
	if (length > 4 && strcasecmp(path + length - 4, ".ppm") == 0)
		return read_ppm(path, picture);

	BBitmap* bitmap = BTranslationUtils::GetBitmap(path);
	if (bitmap == NULL || bitmap->InitCheck() != B_OK
		|| (bitmap->ColorSpace() != B_RGB32
			&& bitmap->ColorSpace() != B_RGBA32)) {
		delete bitmap;
		return false;
	}
	picture.width = bitmap->Bounds().IntegerWidth() + 1;
	picture.height = bitmap->Bounds().IntegerHeight() + 1;
	picture.bytesPerRow = bitmap->BytesPerRow();
	picture.bits.assign((const uint8*)bitmap->Bits(),
		(const uint8*)bitmap->Bits() + bitmap->BitsLength());
	delete bitmap;
	return true;
}


void
write_crop(const std::string& path, const float* tensor)
{
	std::vector<uint8> bgr(kFaceCropSize * kFaceCropSize * 3);
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


struct Found {
	std::string picture;
	int index;
	std::vector<float> embedding;
};

}	// namespace


int
main(int argc, char** argv)
{
	BApplication application("application/x-vnd.airos-Photos-facescan");

	BString models;
	std::string dump;
	std::vector<std::string> pictures;
	FaceScanOptions options = FaceScanOptions::Default();
	uint32 flags = 0;
	bool compare = false, json = false;
	int bench = 0;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		bool more = i + 1 < argc;
		if (arg == "--models" && more)
			models = argv[++i];
		else if (arg == "--cpu")
			flags |= B_NEURAL_CPU_ONLY;
		else if (arg == "--low-power")
			flags |= B_NEURAL_PREFER_LOW_POWER;
		else if (arg == "--tile-scale" && more)
			options.tileScale = atof(argv[++i]);
		else if (arg == "--min-score" && more)
			options.minScore = atof(argv[++i]);
		else if (arg == "--min-quality" && more)
			options.minQuality = atof(argv[++i]);
		else if (arg == "--no-embed")
			options.embed = false;
		else if (arg == "--compare")
			compare = true;
		else if (arg == "--json")
			json = true;
		else if (arg == "--dump" && more)
			dump = argv[++i];
		else if (arg == "--bench") {
			bench = 10;
			if (more && atoi(argv[i + 1]) > 0)
				bench = atoi(argv[++i]);
		} else if (arg[0] == '-' || pictures.size() > 100000) {
			fprintf(stderr, "usage: %s [--models DIR] [--cpu] [--low-power] "
				"[--tile-scale F] [--min-score F] [--min-quality F] "
				"[--no-embed] [--compare] [--dump DIR] [--bench [N]] "
				"[--json] PICTURE...\n", argv[0]);
			return 2;
		} else
			pictures.push_back(arg);
	}
	if (pictures.empty()) {
		fprintf(stderr, "%s: no pictures\n", argv[0]);
		return 2;
	}
	if (models.Length() == 0) {
		BPath path(argv[0]);
		BPath parent;
		if (path.GetParent(&parent) == B_OK) {
			models = parent.Path();
			models << "/models";
		}
		BString probe(models);
		probe << "/face_embedder.tflite";
		if (access(probe.String(), R_OK) != 0)
			models = "models";
	}

	FaceEngine engine;
	bigtime_t start = system_time();
	status_t status = engine.Init(models.String(), flags);
	if (status != B_OK) {
		fprintf(stderr, "%s: the networks in %s could not be set up: %s\n",
			argv[0], models.String(), neural_error_string(status));
		return 1;
	}
	if (!json) {
		printf("networks on %s, set up in %.0f ms; model version %d, "
			"%d numbers per face\n", engine.DeviceName(),
			(system_time() - start) / 1000.0, (int)FaceEngine::kModelVersion,
			(int)engine.EmbeddingSize());
	}

	std::vector<Found> all;
	for (const std::string& name : pictures) {
		Picture picture;
		if (!read_picture(name.c_str(), picture)) {
			fprintf(stderr, "%s: cannot be read as a picture\n", name.c_str());
			continue;
		}
		std::vector<DetectedFace> faces;
		std::vector<FaceCandidate> pixels;
		FaceEngine::Timing best;
		memset(&best, 0, sizeof(best));
		for (int run = 0; run < (bench > 0 ? bench : 1); run++) {
			status = engine.Detect(picture.bits.data(), picture.width,
				picture.height, picture.bytesPerRow, options, faces, &pixels);
			if (status != B_OK)
				break;
			if (run == 0 || engine.LastTiming().total < best.total)
				best = engine.LastTiming();
		}
		if (status != B_OK) {
			fprintf(stderr, "%s: %s\n", name.c_str(), strerror(status));
			continue;
		}

		std::string base = name.substr(name.find_last_of('/') + 1);
		if (json) {
			printf("{\"image\": \"%s\", \"width\": %d, \"height\": %d, "
				"\"ms\": %.1f, \"detect_ms\": %.1f, \"detect_runs\": %d, "
				"\"embed_ms\": %.1f, \"embed_runs\": %d, \"faces\": [",
				name.c_str(), (int)picture.width, (int)picture.height,
				best.total / 1000.0, best.detector / 1000.0,
				(int)best.detectorRuns, best.embedder / 1000.0,
				(int)best.embedderRuns);
		} else {
			printf("%s: %d x %d, %d face%s, %.1f ms%s (detector %d x, "
				"%.1f ms; embedder %d x, %.1f ms)\n", name.c_str(),
				(int)picture.width, (int)picture.height, (int)faces.size(),
				faces.size() == 1 ? "" : "s", best.total / 1000.0,
				bench > 0 ? " at best" : "", (int)best.detectorRuns,
				best.detector / 1000.0, (int)best.embedderRuns,
				best.embedder / 1000.0);
		}
		for (size_t i = 0; i < faces.size(); i++) {
			const DetectedFace& face = faces[i];
			const FaceCandidate& pixel = pixels[i];
			std::vector<float> crop(kFaceCropSize * kFaceCropSize * 3);
			FaceTransform transform;
			float sharpness = 0;
			if (AlignFace(pixel.landmarks, transform)) {
				WarpFace(picture.bits.data(), picture.width, picture.height,
					picture.bytesPerRow, transform, crop.data());
				sharpness = FaceSharpness(crop.data());
				if (!dump.empty()) {
					char suffix[32];
					snprintf(suffix, sizeof(suffix), "-%d.ppm", (int)i);
					write_crop(dump + "/" + base + suffix, crop.data());
				}
			}
			float eyeX = pixel.landmarks[1][0] - pixel.landmarks[0][0];
			float eyeY = pixel.landmarks[1][1] - pixel.landmarks[0][1];
			float eyes = sqrtf(eyeX * eyeX + eyeY * eyeY);
			if (json) {
				printf("%s{\"box\": [%.1f, %.1f, %.1f, %.1f], "
					"\"score\": %.4f, \"quality\": %.4f, \"sharp\": %.4f, "
					"\"frontal\": %.4f, \"eyes\": %.2f, \"lm\": [",
					i > 0 ? ", " : "", pixel.x0, pixel.y0, pixel.x1, pixel.y1,
					face.score, face.quality, sharpness,
					FaceFrontalness(pixel.landmarks), eyes);
				for (int n = 0; n < kFaceLandmarkCount; n++) {
					printf("%s%.1f, %.1f", n > 0 ? ", " : "",
						pixel.landmarks[n][0], pixel.landmarks[n][1]);
				}
				printf("], \"emb\": [");
				for (size_t n = 0; n < face.embedding.size(); n++)
					printf("%s%.5f", n > 0 ? "," : "", face.embedding[n]);
				printf("]}");
			} else {
				printf("  %2d  box %4.0f,%4.0f %4.0f x %4.0f  score %.2f  "
					"quality %.2f (eyes %.0f px, frontal %.2f, sharp %.2f)%s\n",
					(int)i, pixel.x0, pixel.y0, pixel.x1 - pixel.x0,
					pixel.y1 - pixel.y0, face.score, face.quality, eyes,
					FaceFrontalness(pixel.landmarks), sharpness,
					face.embedding.empty() ? "  no embedding" : "");
			}
			if (!face.embedding.empty()) {
				Found found;
				found.picture = base;
				found.index = i;
				found.embedding = face.embedding;
				all.push_back(found);
			}
		}
		if (json)
			printf("]}\n");
		fflush(stdout);
	}

	if (compare && !json) {
		printf("similarities (%.2f and more: the same person; %.2f and more: "
			"maybe)\n", kSameFaceThreshold, kMaybeSameFaceThreshold);
		for (size_t i = 0; i < all.size(); i++) {
			for (size_t j = i + 1; j < all.size(); j++) {
				float similarity = CosineSimilarity(all[i].embedding.data(),
					all[j].embedding.data(), all[i].embedding.size());
				printf("  %s #%d  %s #%d  %.3f%s\n", all[i].picture.c_str(),
					all[i].index, all[j].picture.c_str(), all[j].index,
					similarity, similarity >= kSameFaceThreshold ? "  same"
						: similarity >= kMaybeSameFaceThreshold ? "  maybe"
						: "");
			}
		}
	}
	return 0;
}
