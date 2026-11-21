// Imports run one after the other on a thread: files and folders dropped on
// the window, and pictures from a camera, phone or card.
#pragma once

#include "core/Library.h"

#include <Messenger.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if __has_include("platform/CameraSource.h")
#include "platform/CameraSource.h"
#define PHOTOS_HAVE_CAMERAS 1
#endif

namespace photos {

struct ImportStatus {
	bool running = false;
	int done = 0;
	int total = 0;
	int added = 0;
	int duplicates = 0;
	int failed = 0;
	int64_t importId = 0;
	std::string source;
	std::string name;		// the file at hand
	std::string error;		// the first failure
};

// The key under which a camera's item is remembered as imported.
std::string CameraItemKey(const std::string& name, int64_t size, int64_t taken);

class Importer {
public:
	explicit Importer(Library& library);
	~Importer();

	void SetTarget(const BMessenger& target);
	// Files and folders (searched for pictures).
	void ImportPaths(const std::vector<std::string>& paths, const std::string& source);
#ifdef PHOTOS_HAVE_CAMERAS
	// Downloads the items from an open camera and imports them.
	void ImportFromCamera(std::shared_ptr<CameraSource> camera,
		const std::vector<CameraItem>& items, bool deleteAfterwards);
#endif
	void Cancel();
	ImportStatus Status();
	bool IsBusy();
	// Blocks until everything queued is done (photos_tool).
	void WaitUntilIdle();

private:
	struct Job {
		std::vector<std::string> paths;
		std::string source;
#ifdef PHOTOS_HAVE_CAMERAS
		std::shared_ptr<CameraSource> camera;
		std::vector<CameraItem> items;
#endif
		bool deleteAfterwards = false;
		int generation = 0;
	};

	void _Run();
	void _RunPaths(Job& job);
	void _RunCamera(Job& job);
	void _Collect(const std::string& path, std::vector<std::string>& files, int depth);
	void _Count(const ImportResult& result, const std::string& name);
	void _Report(bool force);

	Library& fLibrary;
	BMessenger fTarget;
	std::mutex fLock;
	std::condition_variable fWake;
	std::condition_variable fIdle;
	std::deque<Job> fJobs;
	ImportStatus fStatus;
	// Cancel() starts a new generation: jobs of older ones stop.
	std::atomic<int> fGeneration;
	std::atomic<int> fCurrent;
	bool _Cancelled() const { return fCurrent != fGeneration; }
	bool fQuit;
	bigtime_t fLastReport;
	std::thread fThread;
};

}	// namespace photos
