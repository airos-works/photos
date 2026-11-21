// Finds the faces in the library's photos on a background thread (Neural
// Kit, FaceEngine) and hands them to People.
#pragma once

#include "core/Library.h"
#include "core/People.h"
#include "platform/Pictures.h"

#include <Messenger.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace photos {

struct FaceScanStatus {
	bool available = false;		// the engine and its models are there
	bool running = false;
	int done = 0;				// photos scanned in this run
	int remaining = 0;
	std::string device;			// what the networks run on
	std::string error;
};

class FaceScanner {
public:
	FaceScanner(Library& library, People& people, Pictures& pictures,
		SyncEngine& sync);
	~FaceScanner();

	void SetTarget(const BMessenger& target);
	// The directory with the models; cpuOnly keeps the networks off GPUs
	// and NPUs. Starts scanning.
	void Start(const std::string& modelDirectory, bool cpuOnly);
	// The same without starting the thread (ScanAllNow).
	void Configure(const std::string& modelDirectory, bool cpuOnly);
	void Stop();
	// There may be new photos.
	void Wake();
	void SetPaused(bool paused);
	FaceScanStatus Status();
	// The engine's model version (photos scanned with another are scanned
	// again); 0 without an engine.
	static int ModelVersion();
	// Scans until nothing is left, or `limit` photos (photos_tool). False if
	// the engine is not available.
	bool ScanAllNow(std::string& error, int limit = 0);

private:
	void _Run();
	bool _ScanOne(void* engine, const PhotoBrief& brief);
	void _Report(bool force);

	Library& fLibrary;
	People& fPeople;
	Pictures& fPictures;
	SyncEngine& fSync;
	BMessenger fTarget;
	std::mutex fLock;
	std::condition_variable fWake;
	std::thread fThread;
	std::string fModels;
	bool fCpuOnly;
	bool fQuit;
	bool fPaused;
	bool fWoken;
	FaceScanStatus fStatus;
	std::set<int64_t> fSkipped;		// could not be loaded in this session
	bigtime_t fLastReport;
};

}	// namespace photos
