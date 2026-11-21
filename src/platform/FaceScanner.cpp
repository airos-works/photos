#include "FaceScanner.h"

#include "ImageIO.h"
#include "Messages.h"

#include <Message.h>
#include <OS.h>

#if __has_include("platform/FaceEngine.h")
#include "core/FaceMath.h"
#include "platform/FaceEngine.h"
#include <NeuralKit.h>
#define PHOTOS_HAVE_FACES 1
#endif

namespace photos {

namespace {

// Faces are looked for in a picture of this size: enough for the faces of a
// group photo, a fraction of the work of the full picture.
const int kScanEdge = 2048;
const int kFaceThumbEdge = 192;

}	// namespace


FaceScanner::FaceScanner(Library& library, People& people, Pictures& pictures,
	SyncEngine& sync)
	:
	fLibrary(library),
	fPeople(people),
	fPictures(pictures),
	fSync(sync),
	fCpuOnly(false),
	fQuit(false),
	fPaused(false),
	fWoken(false),
	fLastReport(0)
{
}


FaceScanner::~FaceScanner()
{
	Stop();
}


int
FaceScanner::ModelVersion()
{
#ifdef PHOTOS_HAVE_FACES
	return (int)FaceEngine::kModelVersion;
#else
	return 0;
#endif
}


void
FaceScanner::SetTarget(const BMessenger& target)
{
	std::lock_guard<std::mutex> lock(fLock);
	fTarget = target;
}


void
FaceScanner::Configure(const std::string& modelDirectory, bool cpuOnly)
{
	std::lock_guard<std::mutex> lock(fLock);
	fModels = modelDirectory;
	fCpuOnly = cpuOnly;
}


void
FaceScanner::Start(const std::string& modelDirectory, bool cpuOnly)
{
	Stop();
	Configure(modelDirectory, cpuOnly);
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = false;
		fWoken = true;
	}
#ifdef PHOTOS_HAVE_FACES
	fThread = std::thread([this]() { _Run(); });
#endif
}


void
FaceScanner::Stop()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = true;
	}
	fWake.notify_all();
	if (fThread.joinable())
		fThread.join();
}


void
FaceScanner::Wake()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fWoken = true;
	}
	fWake.notify_all();
}


void
FaceScanner::SetPaused(bool paused)
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fPaused = paused;
		fWoken = true;
	}
	fWake.notify_all();
}


FaceScanStatus
FaceScanner::Status()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fStatus;
}


void
FaceScanner::_Report(bool force)
{
	bigtime_t now = system_time();
	FaceScanStatus status;
	BMessenger target;
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (!force && now - fLastReport < 500000)
			return;
		fLastReport = now;
		status = fStatus;
		target = fTarget;
	}
	BMessage message(kMsgFacesProgress);
	message.AddBool("running", status.running);
	message.AddBool("available", status.available);
	message.AddInt32("done", status.done);
	message.AddInt32("remaining", status.remaining);
	message.AddString("device", status.device.c_str());
	if (!status.error.empty())
		message.AddString("error", status.error.c_str());
	target.SendMessage(&message);
}


bool
FaceScanner::_ScanOne(void* enginePointer, const PhotoBrief& brief)
{
#ifdef PHOTOS_HAVE_FACES
	FaceEngine* engine = static_cast<FaceEngine*>(enginePointer);
	Photo photo;
	if (!fLibrary.Get(brief.id, photo) || photo.trashed != 0)
		return false;
	Image image;
	std::string error;
	// A photo that is only on the server is scanned from the server's
	// preview, fetched for the purpose and not kept.
	std::string temporary;
	status_t status;
	if (photo.path.empty() && photo.version == 0
		&& !FileExists(fLibrary.PreviewPath(photo.id, photo.revision))) {
		if (photo.immichId.empty())
			return false;
		temporary = JoinPath(fLibrary.Directory(),
			"previews/.scan-" + std::to_string(photo.id) + ".jpg");
		if (!fSync.FetchPreview(photo.immichId, temporary, error))
			return false;
		status = DecodeFile(temporary, image, kScanEdge, kDecodeExact, &error);
		RemoveFile(temporary);
	} else
		status = fPictures.Load(photo, image, kScanEdge, true, false, &error);
	if (status != B_OK || image.IsEmpty())
		return false;

	std::vector<DetectedFace> detected;
	if (engine->Detect(image.pixels.data(), image.width, image.height,
			image.Stride(), detected) != B_OK)
		return false;
	std::vector<Face> faces;
	for (auto& found : detected) {
		// Faces too small or blurred to recognise come without numbers:
		// they are kept (and can be named by hand) but not compared.
		if (found.score < kFaceMinScore)
			continue;
		Face face;
		face.x = found.x;
		face.y = found.y;
		face.width = found.width;
		face.height = found.height;
		face.score = found.score;
		face.quality = found.quality;
		face.embedding = found.embedding;
		faces.push_back(face);
	}
	fPeople.Forget(photo.id);
	fLibrary.SetFaces(photo.id, faces, (int)FaceEngine::kModelVersion);
	// A small picture of each face, for the People view.
	for (auto& face : faces) {
		if (face.id == 0)
			continue;
		float x, y, w, h;
		FaceCropRect(face, image.width, image.height, x, y, w, h);
		Image crop;
		CropImage(image, x, y, w, h, kFaceThumbEdge, kFaceThumbEdge, crop);
		WriteJpeg(crop, 88, fLibrary.FaceThumbPath(face.id));
	}
	fPeople.Place(faces);
	return true;
#else
	(void)enginePointer;
	(void)brief;
	return false;
#endif
}


bool
FaceScanner::ScanAllNow(std::string& error, int limit)
{
#ifdef PHOTOS_HAVE_FACES
	FaceEngine engine;
	status_t status = engine.Init(fModels.c_str(), fCpuOnly ? B_NEURAL_CPU_ONLY : 0);
	if (status != B_OK) {
		error = std::string("cannot start the face engine: ") + strerror(status);
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(fLock);
		fStatus.available = true;
		fStatus.device = engine.DeviceName();
	}
	std::vector<PhotoBrief> photos;
	fLibrary.List(kFilterNeedsFaces, FaceEngine::kModelVersion, photos);
	int scanned = 0;
	for (auto& brief : photos) {
		if (limit > 0 && scanned >= limit)
			break;
		if (_ScanOne(&engine, brief)) {
			std::lock_guard<std::mutex> lock(fLock);
			fStatus.done++;
			scanned++;
		}
	}
	return true;
#else
	error = "this build has no face engine";
	return false;
#endif
}


void
FaceScanner::_Run()
{
#ifdef PHOTOS_HAVE_FACES
	set_thread_priority(find_thread(nullptr), B_LOW_PRIORITY);
	FaceEngine* engine = nullptr;
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(fLock);
			fWake.wait(lock, [this]() { return fQuit || (fWoken && !fPaused); });
			if (fQuit)
				break;
			fWoken = false;
		}
		std::vector<PhotoBrief> photos;
		fLibrary.List(kFilterNeedsFaces, FaceEngine::kModelVersion, photos);
		{
			std::lock_guard<std::mutex> lock(fLock);
			for (auto it = photos.begin(); it != photos.end();) {
				if (fSkipped.count(it->id) != 0)
					it = photos.erase(it);
				else
					++it;
			}
		}
		if (photos.empty())
			continue;
		if (engine == nullptr) {
			// Loading the networks takes a moment and memory: only now that
			// there is something to scan.
			std::string models;
			bool cpuOnly;
			{
				std::lock_guard<std::mutex> lock(fLock);
				models = fModels;
				cpuOnly = fCpuOnly;
			}
			engine = new FaceEngine();
			status_t status = engine->Init(models.c_str(),
				cpuOnly ? B_NEURAL_CPU_ONLY : 0);
			std::lock_guard<std::mutex> lock(fLock);
			if (status != B_OK) {
				fStatus.available = false;
				fStatus.error = std::string("The face models could not be loaded: ")
					+ strerror(status);
				delete engine;
				engine = nullptr;
			} else {
				fStatus.available = true;
				fStatus.error.clear();
				fStatus.device = engine->DeviceName();
			}
		}
		if (engine == nullptr) {
			_Report(true);
			// Not again until asked (settings changed, restart).
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(fLock);
			fStatus.running = true;
			fStatus.remaining = (int)photos.size();
		}
		_Report(true);
		for (size_t i = 0; i < photos.size(); i++) {
			{
				std::lock_guard<std::mutex> lock(fLock);
				if (fQuit || fPaused)
					break;
			}
			bool ok = _ScanOne(engine, photos[i]);
			{
				std::lock_guard<std::mutex> lock(fLock);
				if (ok)
					fStatus.done++;
				else
					fSkipped.insert(photos[i].id);
				fStatus.remaining = (int)(photos.size() - i - 1);
			}
			_Report(false);
		}
		{
			std::lock_guard<std::mutex> lock(fLock);
			fStatus.running = false;
		}
		_Report(true);
	}
	delete engine;
#endif
}

}	// namespace photos
