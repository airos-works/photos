#include "Importer.h"

#include "Messages.h"

#include <Message.h>
#include <OS.h>

#include <sys/stat.h>

namespace photos {

std::string
CameraItemKey(const std::string& name, int64_t size, int64_t taken)
{
	return ToLower(name) + "|" + std::to_string(size) + "|" + std::to_string(taken);
}


Importer::Importer(Library& library)
	:
	fLibrary(library),
	fGeneration(0),
	fCurrent(0),
	fQuit(false),
	fLastReport(0)
{
	fThread = std::thread([this]() { _Run(); });
}


Importer::~Importer()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = true;
		fGeneration++;
	}
	fWake.notify_all();
	fThread.join();
}


void
Importer::SetTarget(const BMessenger& target)
{
	std::lock_guard<std::mutex> lock(fLock);
	fTarget = target;
}


void
Importer::ImportPaths(const std::vector<std::string>& paths, const std::string& source)
{
	Job job;
	job.paths = paths;
	job.source = source;
	{
		std::lock_guard<std::mutex> lock(fLock);
		job.generation = fGeneration;
		fJobs.push_back(job);
		fStatus.running = true;
	}
	fWake.notify_all();
}


#ifdef PHOTOS_HAVE_CAMERAS
void
Importer::ImportFromCamera(std::shared_ptr<CameraSource> camera,
	const std::vector<CameraItem>& items, bool deleteAfterwards)
{
	Job job;
	job.camera = camera;
	job.items = items;
	job.source = camera->Info().name;
	job.deleteAfterwards = deleteAfterwards;
	{
		std::lock_guard<std::mutex> lock(fLock);
		job.generation = fGeneration;
		fJobs.push_back(job);
		fStatus.running = true;
	}
	fWake.notify_all();
}
#endif


void
Importer::Cancel()
{
	std::lock_guard<std::mutex> lock(fLock);
	fJobs.clear();
	fGeneration++;
}


ImportStatus
Importer::Status()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fStatus;
}


bool
Importer::IsBusy()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fStatus.running;
}


void
Importer::WaitUntilIdle()
{
	std::unique_lock<std::mutex> lock(fLock);
	fIdle.wait(lock, [this]() { return !fStatus.running; });
}


void
Importer::_Report(bool force)
{
	bigtime_t now = system_time();
	ImportStatus status;
	BMessenger target;
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (!force && now - fLastReport < 150000)
			return;
		fLastReport = now;
		status = fStatus;
		target = fTarget;
	}
	BMessage message(kMsgImportProgress);
	message.AddBool("running", status.running);
	message.AddInt32("done", status.done);
	message.AddInt32("total", status.total);
	message.AddInt32("added", status.added);
	message.AddInt32("duplicates", status.duplicates);
	message.AddInt32("failed", status.failed);
	message.AddInt64("import", status.importId);
	message.AddString("source", status.source.c_str());
	message.AddString("name", status.name.c_str());
	if (!status.error.empty())
		message.AddString("error", status.error.c_str());
	target.SendMessage(&message);
}


void
Importer::_Count(const ImportResult& result, const std::string& name)
{
	std::lock_guard<std::mutex> lock(fLock);
	fStatus.done++;
	if (result.added || result.restored)
		fStatus.added++;
	else if (result.duplicate)
		fStatus.duplicates++;
	else if (result.id == 0) {
		fStatus.failed++;
		if (fStatus.error.empty())
			fStatus.error = name + ": " + result.error;
	}
}


void
Importer::_Collect(const std::string& path, std::vector<std::string>& files, int depth)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		if (depth > 12)
			return;
		// Not into another library.
		if (FileExists(JoinPath(path, "library.db")))
			return;
		std::vector<std::string> names;
		ListDirectory(path, names);
		for (auto& name : names) {
			if (name.empty() || name[0] == '.')
				continue;
			_Collect(JoinPath(path, name), files, depth + 1);
		}
	} else if (S_ISREG(st.st_mode) && IsPictureName(path))
		files.push_back(path);
}


void
Importer::_RunPaths(Job& job)
{
	std::vector<std::string> files;
	for (auto& path : job.paths)
		_Collect(path, files, 0);
	int64_t importId = files.empty() ? 0 : fLibrary.BeginImport(job.source);
	{
		std::lock_guard<std::mutex> lock(fLock);
		fStatus.total += (int)files.size();
		fStatus.importId = importId;
		fStatus.source = job.source;
	}
	for (auto& file : files) {
		if (_Cancelled())
			break;
		{
			std::lock_guard<std::mutex> lock(fLock);
			fStatus.name = BaseName(file);
		}
		_Report(false);
		ImportOptions options;
		options.importId = importId;
		ImportResult result = fLibrary.ImportFile(file, options);
		_Count(result, BaseName(file));
	}
}


void
Importer::_RunCamera(Job& job)
{
#ifdef PHOTOS_HAVE_CAMERAS
	int64_t importId = job.items.empty() ? 0 : fLibrary.BeginImport(job.source);
	{
		std::lock_guard<std::mutex> lock(fLock);
		fStatus.total += (int)job.items.size();
		fStatus.importId = importId;
		fStatus.source = job.source;
	}
	std::string incoming = JoinPath(fLibrary.Directory(), "originals/.incoming");
	MakeDirs(incoming);
	for (auto& item : job.items) {
		if (_Cancelled())
			break;
		{
			std::lock_guard<std::mutex> lock(fLock);
			fStatus.name = item.name;
		}
		_Report(false);
		ImportResult result;
		if (item.isVideo || !IsPictureName(item.name)) {
			result.error = "not a picture";
			_Count(result, item.name);
			continue;
		}
		std::string temporary = JoinPath(incoming, RandomId() + "-" + item.name);
		status_t status = job.camera->Download(item, temporary.c_str(),
			[this](int64, int64) { return !_Cancelled(); });
		if (status != B_OK) {
			RemoveFile(temporary);
			if (_Cancelled())
				break;
			result.error = strerror(status);
			_Count(result, item.name);
			continue;
		}
		ImportOptions options;
		options.importId = importId;
		options.move = true;
		options.name = item.name;
		options.takenFallback = item.taken;
		options.itemKey = CameraItemKey(item.name, item.size, item.taken);
		result = fLibrary.ImportFile(temporary, options);
		RemoveFile(temporary);
		_Count(result, item.name);
		if (result.id != 0 && job.deleteAfterwards)
			job.camera->DeleteItem(item);
	}
#else
	(void)job;
#endif
}


void
Importer::_Run()
{
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(fLock);
			if (fJobs.empty()) {
				if (fStatus.running) {
					fStatus.running = false;
					fStatus.name.clear();
					lock.unlock();
					_Report(true);
					fIdle.notify_all();
					lock.lock();
				}
				fWake.wait(lock, [this]() { return fQuit || !fJobs.empty(); });
				if (fQuit)
					return;
				// A new run of jobs starts counting from nothing.
				fStatus = ImportStatus();
				fStatus.running = true;
			}
			job = fJobs.front();
			fJobs.pop_front();
			fCurrent = job.generation;
		}
#ifdef PHOTOS_HAVE_CAMERAS
		if (job.camera)
			_RunCamera(job);
		else
#endif
			_RunPaths(job);
		_Report(true);
	}
}

}	// namespace photos
