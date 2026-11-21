// Faces with the Neural Kit: finds the faces of a picture and gives each a
// vector that is close to the vectors of the same person's other faces.
// The arithmetic around the two networks is core/FaceMath (which also has
// the thresholds); docs/FACES.md has the models and the measurements.
#pragma once

#include <SupportDefs.h>

#include <vector>

#include "core/FaceMath.h"		// DetectedFace, the thresholds

namespace photos {

class FaceEngine {
public:
	// Goes up when the models or what is done around them change in a way
	// that changes embeddings: faces found with another version are to be
	// searched for again (vectors of different versions do not compare).
	static const int32 kModelVersion;

								FaceEngine();
								~FaceEngine();

	// Loads the networks (face_detector_640.tflite, face_embedder.tflite
	// and, if it is there, face_detector_1280.tflite for the small faces
	// of larger pictures, in modelDirectory) onto a device: flags are the
	// Neural Kit's
	// (0: its choice, a graphics processor when there is one;
	// B_NEURAL_CPU_ONLY; B_NEURAL_PREFER_LOW_POWER). Takes some tenths of
	// a second, more on a graphics processor: keep the engine. A device
	// whose answers differ from the CPU's on a test picture is not used.
			status_t			Init(const char* modelDirectory,
									uint32 flags = 0);
			bool				IsReady() const;

	// A B_RGB32-layout buffer (bytes blue, green, red, any) of any size,
	// upright. Blocks until done; an engine does one picture at a time
	// (a second caller waits), several engines work at the same time.
	// Faces come best first; see DetectedFace for what is in them.
			status_t			Detect(const uint8* bits, int32 width,
									int32 height, int32 bytesPerRow,
									std::vector<DetectedFace>& faces);
	// The same with other options than FaceScanOptions::Default(), and
	// the faces in pixels as well (for tools).
			status_t			Detect(const uint8* bits, int32 width,
									int32 height, int32 bytesPerRow,
									const FaceScanOptions& options,
									std::vector<DetectedFace>& faces,
									std::vector<FaceCandidate>* pixels = NULL);

			int32				EmbeddingSize() const;
	// "CPU", "AMD Radeon 8060S Graphics (GPU)": where the networks run,
	// for showing.
			const char*			DeviceName() const;

	// Times of the last Detect(), in microseconds, and how many times each
	// network ran in it.
			struct Timing {
				bigtime_t		total;
				bigtime_t		detector;
				bigtime_t		embedder;
				int32			detectorRuns;
				int32			embedderRuns;
			};
			const Timing&		LastTiming() const;

private:
			class Networks;

								FaceEngine(const FaceEngine&);
			FaceEngine&			operator=(const FaceEngine&);

			Networks*			fNetworks;
			Timing				fTiming;
};

}	// namespace photos
