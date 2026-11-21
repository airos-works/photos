#include "platform/CameraSource.h"

#include "platform/UsbPtpTransport.h"
#include "ptp/Ptp.h"

#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <Locker.h>
#include <Looper.h>
#include <NodeMonitor.h>
#include <Path.h>
#include <USBKit.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace photos {

namespace {

const size_t kExifProbeSize = 64 * 1024;
const size_t kCopyChunk = 1024 * 1024;
const int kMaxFolderDepth = 12;


// A time a camera wrote without a zone, as the computer's local time.
int64_t
LocalTime(int64_t wallClockAsUtc)
{
	time_t seconds = (time_t)wallClockAsUtc;
	struct tm fields;
	if (gmtime_r(&seconds, &fields) == NULL)
		return wallClockAsUtc;
	fields.tm_isdst = -1;
	time_t local = mktime(&fields);
	return local == (time_t)-1 ? wallClockAsUtc : (int64_t)local;
}


status_t
StatusFor(PtpStatus status)
{
	switch (status) {
		case kPtpOk:
			return B_OK;
		case kPtpIoError:
			return B_IO_ERROR;
		case kPtpTimeout:
			return B_TIMED_OUT;
		case kPtpNoDevice:
			return B_DEV_NOT_READY;
		case kPtpProtocol:
			return B_BAD_DATA;
		case kPtpCancelled:
			return B_CANCELED;
		case kPtpAccessDenied:
			return B_PERMISSION_DENIED;
		case kPtpBusy:
			return B_BUSY;
		case kPtpNotSupported:
			return B_UNSUPPORTED;
		case kPtpStoreUnavailable:
			return B_DEV_NO_MEDIA;
		case kPtpInvalidObject:
			return B_ENTRY_NOT_FOUND;
		case kPtpReadOnly:
			return B_READ_ONLY_DEVICE;
		case kPtpNoSession:
			return B_NO_INIT;
		case kPtpFailed:
			break;
	}
	return B_ERROR;
}


void
SetModificationTime(const char* path, int64_t taken)
{
	if (taken <= 0)
		return;
	struct timeval times[2];
	times[0].tv_sec = times[1].tv_sec = (time_t)taken;
	times[0].tv_usec = times[1].tv_usec = 0;
	utimes(path, times);
}


bool
WriteAll(int fd, const uint8_t* data, size_t size)
{
	while (size > 0) {
		ssize_t written = write(fd, data, size);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		data += written;
		size -= (size_t)written;
	}
	return true;
}


bool
IsJpegName(const std::string& name)
{
	size_t dot = name.rfind('.');
	if (dot == std::string::npos)
		return false;
	const char* extension = name.c_str() + dot + 1;
	return strcasecmp(extension, "jpg") == 0
		|| strcasecmp(extension, "jpeg") == 0
		|| strcasecmp(extension, "jpe") == 0;
}


// The folders in the root of a camera's or phone's storage that hold what
// a person calls its pictures.
bool
IsCameraFolder(const std::string& name)
{
	return strcasecmp(name.c_str(), "DCIM") == 0
		|| strcasecmp(name.c_str(), "Pictures") == 0;
}


// #pragma mark - PTP/MTP


class PtpCameraSource : public CameraSource {
public:
	PtpCameraSource(const CameraInfo& info, const std::string& path)
		:
		fInfo(info),
		fPath(path),
		fSession(NULL),
		fLock("ptp camera")
	{
	}

	virtual ~PtpCameraSource()
	{
		Close();
	}

	virtual const CameraInfo& Info() const { return fInfo; }

	virtual std::string LastError() const
	{
		BAutolock locker(fLock);
		return fError;
	}

	virtual status_t Open(std::string* error)
	{
		BAutolock locker(fLock);
		_Close();
		fError.clear();
		status_t status = _Open();
		if (status != B_OK)
			_Close();
		if (error != NULL)
			*error = fError;
		return status;
	}

	virtual status_t ListItems(std::vector<CameraItem>& items,
		const std::atomic<bool>* cancel,
		const std::function<void(int32 found)>& progress)
	{
		BAutolock locker(fLock);
		items.clear();
		if (fSession == NULL)
			return _Fail(kPtpNoSession);

		// A phone holds the pictures of every program on it, their caches
		// too. What a person means by "the pictures on the phone" is in
		// DCIM (the camera) and Pictures (screenshots, saved pictures):
		// only those are looked into, where the device can be walked, and
		// only what is in them is listed. A device with neither is taken
		// whole.
		PtpFolderFilter enter = [](const PtpObjectInfo& folder, int depth) {
			if (depth == 0)
				return IsCameraFolder(folder.filename);
			return !folder.filename.empty() && folder.filename[0] != '.';
		};
		auto report = [&](int found) {
			if (progress)
				progress(found);
		};
		std::vector<PtpObjectInfo> objects;
		PtpStatus status = fSession->ListObjects(objects, cancel, report,
			enter);
		if (status != kPtpOk)
			return _Fail(status);
		bool cameraFolders = false;
		for (const PtpObjectInfo& object : objects) {
			if (object.IsFolder() && _IsRoot(object.parent)
				&& IsCameraFolder(object.filename)) {
				cameraFolders = true;
			}
		}
		if (!cameraFolders) {
			status = fSession->ListObjects(objects, cancel, report);
			if (status != kPtpOk)
				return _Fail(status);
		}

		std::unordered_map<uint32, size_t> index;
		for (size_t i = 0; i < objects.size(); i++)
			index[objects[i].handle] = i;

		for (const PtpObjectInfo& object : objects) {
			if (object.IsFolder())
				continue;
			PtpMediaKind kind = PtpMediaKindOf(object.filename, object.format);
			if (kind == kPtpMediaOther || object.filename[0] == '.')
				continue;

			CameraItem item;
			char id[16];
			snprintf(id, sizeof(id), "%08x", (unsigned)object.handle);
			item.id = id;
			item.name = object.filename;
			item.size = object.size == kPtpSizeUnknown ? 0
				: (int64_t)object.size;
			item.isVideo = kind == kPtpMediaVideo;
			item.isRaw = kind == kPtpMediaRaw;
			if (object.captured != 0) {
				item.taken = object.capturedIsUtc ? object.captured
					: LocalTime(object.captured);
			} else if (object.modified != 0) {
				item.taken = object.modified
					? (object.modifiedIsUtc ? object.modified
						: LocalTime(object.modified))
					: 0;
			}

			// The folders above it, the outermost first.
			uint32 parent = object.parent;
			bool hidden = false;
			std::string top;
			for (int depth = 0; !_IsRoot(parent) && depth < kMaxFolderDepth;
					depth++) {
				auto found = index.find(parent);
				if (found == index.end())
					break;
				const PtpObjectInfo& folder = objects[found->second];
				item.folder = item.folder.empty() ? folder.filename
					: folder.filename + "/" + item.folder;
				hidden = hidden || folder.filename[0] == '.';
				top = folder.filename;
				parent = folder.parent;
			}
			if (hidden || (cameraFolders && !IsCameraFolder(top)))
				continue;
			items.push_back(item);
		}
		fError.clear();
		return B_OK;
	}

	virtual status_t GetThumbnail(const CameraItem& item,
		std::vector<uint8_t>& jpeg)
	{
		BAutolock locker(fLock);
		jpeg.clear();
		if (fSession == NULL)
			return _Fail(kPtpNoSession);
		uint32 handle = (uint32)strtoul(item.id.c_str(), NULL, 16);
		const PtpDeviceInfo& device = fSession->DeviceInfo();
		if (device.Supports(kPtpOpGetThumb)) {
			PtpStatus status = fSession->GetThumb(handle, jpeg);
			if (status == kPtpOk && jpeg.size() > 4 && jpeg[0] == 0xFF
				&& jpeg[1] == 0xD8) {
				fError.clear();
				return B_OK;
			}
			if (status == kPtpNoDevice || status == kPtpTimeout
				|| status == kPtpIoError) {
				return _Fail(status);
			}
			jpeg.clear();
		}
		// The thumbnail the camera put into the file itself.
		if (device.Supports(kPtpOpGetPartialObject) && IsJpegName(item.name)) {
			std::vector<uint8_t> head;
			PtpStatus status = fSession->GetPartialObject(handle, 0,
				kExifProbeSize, head);
			if (status == kPtpOk
				&& ExifThumbnail(head.data(), head.size(), jpeg)) {
				fError.clear();
				return B_OK;
			}
			if (status == kPtpNoDevice || status == kPtpTimeout
				|| status == kPtpIoError) {
				return _Fail(status);
			}
		}
		jpeg.clear();
		fError.clear();
		return B_UNSUPPORTED;
	}

	virtual status_t Download(const CameraItem& item, const char* path,
		const std::function<bool(int64 done, int64 total)>& progress)
	{
		BAutolock locker(fLock);
		if (fSession == NULL)
			return _Fail(kPtpNoSession);
		uint32 handle = (uint32)strtoul(item.id.c_str(), NULL, 16);
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd < 0) {
			status_t status = errno;
			fError = std::string("The file could not be written: ")
				+ strerror(status);
			return status;
		}

		int64 done = 0;
		status_t writeError = B_OK;
		PtpDataSink sink = [&](const uint8_t* data, size_t size,
				uint64_t total) {
			if (!WriteAll(fd, data, size)) {
				writeError = errno != 0 ? errno : B_IO_ERROR;
				return false;
			}
			done += size;
			int64 all = total == kPtpSizeUnknown ? item.size : (int64)total;
			return !progress || progress(done, all);
		};
		PtpStatus status = fSession->GetObject(handle, sink,
			item.size > 0 ? (uint64_t)item.size : kPtpSizeUnknown);
		if (close(fd) != 0 && status == kPtpOk && writeError == B_OK)
			writeError = errno;

		if (writeError != B_OK) {
			unlink(path);
			fError = std::string("The file could not be written: ")
				+ strerror(writeError);
			return writeError;
		}
		if (status != kPtpOk) {
			unlink(path);
			return _Fail(status);
		}
		SetModificationTime(path, item.taken);
		fError.clear();
		return B_OK;
	}

	virtual status_t DeleteItem(const CameraItem& item)
	{
		BAutolock locker(fLock);
		if (fSession == NULL)
			return _Fail(kPtpNoSession);
		uint32 handle = (uint32)strtoul(item.id.c_str(), NULL, 16);
		PtpStatus status = fSession->DeleteObject(handle);
		if (status != kPtpOk)
			return _Fail(status);
		fError.clear();
		return B_OK;
	}

	virtual void Close()
	{
		BAutolock locker(fLock);
		_Close();
	}

private:
	static bool _IsRoot(uint32 parent)
	{
		return parent == 0 || parent == 0xFFFFFFFF;
	}

	status_t _Open()
	{
		status_t status = fTransport.Open(fPath.c_str());
		if (status != B_OK) {
			fError = fInfo.name + " is not connected any more.";
			return B_DEV_NOT_READY;
		}
		fSession = new(std::nothrow) PtpSession(fTransport);
		if (fSession == NULL)
			return B_NO_MEMORY;
		PtpStatus opened = fSession->Open();
		if (opened != kPtpOk)
			return _Fail(opened);

		const PtpDeviceInfo& device = fSession->DeviceInfo();
		if (!device.model.empty())
			fInfo.name = device.model;

		// A phone shows its storage once it is unlocked and its owner has
		// said yes; a camera needs a card.
		std::vector<uint32_t> ids;
		PtpStatus status2 = fSession->GetStorageIDs(ids);
		if (status2 != kPtpOk)
			return _Fail(status2);
		bool usable = false;
		for (uint32_t id : ids)
			usable = usable || (id & 0xFFFF) != 0;
		if (!usable) {
			bool phone = fTransport.IsMtpInterface() || device.IsMtp()
				|| device.manufacturer.find("Apple") != std::string::npos;
			return _Fail(phone ? kPtpAccessDenied : kPtpStoreUnavailable);
		}
		return B_OK;
	}

	void _Close()
	{
		if (fSession != NULL) {
			fSession->Close();
			delete fSession;
			fSession = NULL;
		}
		fTransport.Close();
	}

	status_t _Fail(PtpStatus status)
	{
		switch (status) {
			case kPtpAccessDenied:
				fError = fInfo.name + " is locked or has not allowed access "
					"to its pictures yet. Unlock it, answer \"Allow\" or "
					"\"Trust\" when it asks, and try again.";
				break;
			case kPtpStoreUnavailable:
				fError = fInfo.name + " has no memory card, or its storage "
					"is not available.";
				break;
			case kPtpNoDevice:
				fError = fInfo.name + " was disconnected.";
				break;
			case kPtpTimeout:
				fError = fInfo.name + " did not answer. Disconnect it and "
					"connect it again.";
				break;
			case kPtpBusy:
				fError = fInfo.name + " is busy. Try again in a moment.";
				break;
			case kPtpCancelled:
				fError.clear();
				break;
			default:
				fError = std::string(PtpStatusText(status)) + ".";
				if (fSession != NULL && fSession->LastResponse() != 0
					&& fSession->LastResponse() != kPtpRespOk) {
					fError += " (" + fSession->LastError() + ")";
				}
				break;
		}
		return StatusFor(status);
	}

	CameraInfo			fInfo;
	std::string			fPath;
	UsbPtpTransport		fTransport;
	PtpSession*			fSession;
	mutable BLocker		fLock;
	std::string			fError;
};


// #pragma mark - volumes


// The name of the DCIM folder in a volume's root as it is spelled there.
bool
FindDcim(const char* root, std::string* name)
{
	DIR* directory = opendir(root);
	if (directory == NULL)
		return false;
	bool found = false;
	while (struct dirent* entry = readdir(directory)) {
		if (strcasecmp(entry->d_name, "DCIM") != 0)
			continue;
		std::string path = std::string(root) + "/" + entry->d_name;
		struct stat st;
		if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
			if (name != NULL)
				*name = entry->d_name;
			found = true;
			break;
		}
	}
	closedir(directory);
	return found;
}


class VolumeCameraSource : public CameraSource {
public:
	VolumeCameraSource(const CameraInfo& info, const std::string& root)
		:
		fInfo(info),
		fRoot(root),
		fOpen(false),
		fLock("volume camera")
	{
	}

	virtual const CameraInfo& Info() const { return fInfo; }

	virtual std::string LastError() const
	{
		BAutolock locker(fLock);
		return fError;
	}

	virtual status_t Open(std::string* error)
	{
		BAutolock locker(fLock);
		fOpen = FindDcim(fRoot.c_str(), &fDcim);
		fError = fOpen ? "" : fInfo.name + " is not there any more.";
		if (error != NULL)
			*error = fError;
		return fOpen ? B_OK : B_DEV_NOT_READY;
	}

	virtual status_t ListItems(std::vector<CameraItem>& items,
		const std::atomic<bool>* cancel,
		const std::function<void(int32 found)>& progress)
	{
		BAutolock locker(fLock);
		items.clear();
		if (!fOpen)
			return _Fail(B_NO_INIT, "listed");
		status_t status = _Walk(fDcim, 0, items, cancel, progress);
		if (status != B_OK) {
			items.clear();
			if (status == B_CANCELED) {
				fError.clear();
				return status;
			}
			return _Fail(status, "read");
		}
		std::sort(items.begin(), items.end(),
			[](const CameraItem& a, const CameraItem& b) {
				return a.id < b.id;
			});
		if (progress)
			progress((int32)items.size());
		fError.clear();
		return B_OK;
	}

	virtual status_t GetThumbnail(const CameraItem& item,
		std::vector<uint8_t>& jpeg)
	{
		BAutolock locker(fLock);
		jpeg.clear();
		fError.clear();
		if (!fOpen)
			return B_NO_INIT;
		if (!IsJpegName(item.name))
			return B_UNSUPPORTED;
		std::string path = fRoot + "/" + item.id;
		int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return _Fail(errno, "read");
		std::vector<uint8_t> head(kExifProbeSize);
		ssize_t got = read(fd, head.data(), head.size());
		close(fd);
		if (got <= 0)
			return B_UNSUPPORTED;
		return ExifThumbnail(head.data(), (size_t)got, jpeg) ? B_OK
			: B_UNSUPPORTED;
	}

	virtual status_t Download(const CameraItem& item, const char* path,
		const std::function<bool(int64 done, int64 total)>& progress)
	{
		BAutolock locker(fLock);
		if (!fOpen)
			return _Fail(B_NO_INIT, "read");
		std::string source = fRoot + "/" + item.id;
		int in = open(source.c_str(), O_RDONLY | O_CLOEXEC);
		if (in < 0)
			return _Fail(errno, "read");
		struct stat st;
		int64 total = fstat(in, &st) == 0 ? (int64)st.st_size : item.size;
		int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (out < 0) {
			status_t status = errno;
			close(in);
			fError = std::string("The file could not be written: ")
				+ strerror(status);
			return status;
		}

		std::vector<uint8_t> buffer(kCopyChunk);
		int64 done = 0;
		status_t status = B_OK;
		bool writing = false;
		while (true) {
			ssize_t got = read(in, buffer.data(), buffer.size());
			if (got < 0) {
				if (errno == EINTR)
					continue;
				status = errno;
				break;
			}
			if (got == 0)
				break;
			if (!WriteAll(out, buffer.data(), (size_t)got)) {
				status = errno != 0 ? errno : B_IO_ERROR;
				writing = true;
				break;
			}
			done += got;
			if (progress && !progress(done, total)) {
				status = B_CANCELED;
				break;
			}
		}
		close(in);
		if (close(out) != 0 && status == B_OK) {
			status = errno;
			writing = true;
		}
		if (status == B_OK && done != total) {
			// The card went away under the copy.
			status = B_IO_ERROR;
		}
		if (status != B_OK) {
			unlink(path);
			if (status == B_CANCELED) {
				fError.clear();
				return status;
			}
			if (writing) {
				fError = std::string("The file could not be written: ")
					+ strerror(status);
				return status;
			}
			return _Fail(status, "read");
		}
		SetModificationTime(path, item.taken);
		fError.clear();
		return B_OK;
	}

	virtual status_t DeleteItem(const CameraItem& item)
	{
		BAutolock locker(fLock);
		if (!fOpen)
			return _Fail(B_NO_INIT, "changed");
		std::string path = fRoot + "/" + item.id;
		if (unlink(path.c_str()) != 0)
			return _Fail(errno, "changed");
		fError.clear();
		return B_OK;
	}

	virtual void Close()
	{
		BAutolock locker(fLock);
		fOpen = false;
	}

private:
	status_t _Fail(status_t status, const char* what)
	{
		struct stat st;
		if (stat(fRoot.c_str(), &st) != 0 || !FindDcim(fRoot.c_str(), NULL)) {
			fError = fInfo.name + " was removed.";
			return B_DEV_NOT_READY;
		}
		fError = fInfo.name + " could not be " + what + ": "
			+ strerror(status) + ".";
		return status;
	}

	status_t _Walk(const std::string& relative, int depth,
		std::vector<CameraItem>& items, const std::atomic<bool>* cancel,
		const std::function<void(int32 found)>& progress)
	{
		std::string path = fRoot + "/" + relative;
		DIR* directory = opendir(path.c_str());
		if (directory == NULL)
			return depth == 0 ? (status_t)errno : B_OK;
		status_t status = B_OK;
		std::vector<std::string> folders;
		while (struct dirent* entry = readdir(directory)) {
			if (cancel != NULL && *cancel) {
				status = B_CANCELED;
				break;
			}
			const char* name = entry->d_name;
			// ".", "..", and what other systems leave behind (".Trashes",
			// "._IMG_0001.JPG")
			if (name[0] == '.')
				continue;
			std::string child = relative + "/" + name;
			struct stat st;
			if (stat((fRoot + "/" + child).c_str(), &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode)) {
				if (depth + 1 < kMaxFolderDepth)
					folders.push_back(child);
				continue;
			}
			if (!S_ISREG(st.st_mode))
				continue;
			PtpMediaKind kind = PtpMediaKindOf(name);
			if (kind == kPtpMediaOther)
				continue;
			CameraItem item;
			item.id = child;
			item.name = name;
			item.folder = relative;
			item.size = (int64_t)st.st_size;
			item.taken = (int64_t)st.st_mtime;
			item.isVideo = kind == kPtpMediaVideo;
			item.isRaw = kind == kPtpMediaRaw;
			items.push_back(item);
			if (progress && items.size() % 32 == 0)
				progress((int32)items.size());
		}
		closedir(directory);
		for (size_t i = 0; status == B_OK && i < folders.size(); i++)
			status = _Walk(folders[i], depth + 1, items, cancel, progress);
		return status;
	}

	CameraInfo			fInfo;
	std::string			fRoot;
	std::string			fDcim;
	bool				fOpen;
	mutable BLocker		fLock;
	std::string			fError;
};

}	// namespace


// #pragma mark - CameraWatcher


class CameraWatcher::Impl : public BUSBRoster {
public:
	Impl(const BMessenger& target)
		:
		fTarget(target),
		fLock("camera watcher"),
		fLooper(NULL),
		fStarted(false),
		fInitial(false),
		fStopping(false)
	{
	}

	virtual ~Impl()
	{
		StopWatching();
	}

	status_t StartWatching()
	{
		if (fStarted)
			return B_OK;
		fStopping = false;
		fInitial = true;

		fLooper = new(std::nothrow) VolumeLooper(this);
		if (fLooper == NULL)
			return B_NO_MEMORY;
		fLooper->Run();
		status_t status = fVolumeRoster.StartWatching(BMessenger(fLooper));
		if (status != B_OK) {
			fLooper->Lock();
			fLooper->Quit();
			fLooper = NULL;
			return status;
		}
		BVolume volume;
		fVolumeRoster.Rewind();
		while (fVolumeRoster.GetNextVolume(&volume) == B_OK)
			VolumeMounted(volume.Device());

		// Looks at every device that is there before it returns, then
		// watches from a thread of its own.
		BUSBRoster::Start();
		fInitial = false;
		fStarted = true;
		return B_OK;
	}

	void StopWatching()
	{
		if (!fStarted && fLooper == NULL)
			return;
		fStopping = true;
		BUSBRoster::Stop();
		fVolumeRoster.StopWatching();
		if (fLooper != NULL) {
			fLooper->Lock();
			fLooper->Quit();
			fLooper = NULL;
		}
		BAutolock locker(fLock);
		fEntries.clear();
		fStarted = false;
	}

	virtual status_t DeviceAdded(BUSBDevice* device)
	{
		if (device == NULL || device->InitCheck() != B_OK || device->IsHub())
			return B_ERROR;
		UsbPtpLocation location;
		if (!FindUsbPtpInterface(*device, &location))
			return B_ERROR;

		Entry entry;
		entry.usb = device;
		entry.path = std::string("/dev/bus/usb/")
			+ (device->Location() != NULL ? device->Location() : "");
		entry.info.kind = kCameraPtp;
		const char* product = device->ProductString();
		const char* manufacturer = device->ManufacturerString();
		if (product != NULL && product[0] != '\0')
			entry.info.name = product;
		else if (manufacturer != NULL && manufacturer[0] != '\0')
			entry.info.name = manufacturer;
		else
			entry.info.name = location.mtp ? "Phone" : "Camera";
		const char* serial = device->SerialNumberString();
		char key[64];
		snprintf(key, sizeof(key), "usb:%04x:%04x:", device->VendorID(),
			device->ProductID());
		entry.info.key = key;
		entry.info.key += serial != NULL && serial[0] != '\0' ? serial
			: (device->Location() != NULL ? device->Location() : "");
		_Add(entry);
		return B_OK;
	}

	virtual void DeviceRemoved(BUSBDevice* device)
	{
		BAutolock locker(fLock);
		for (size_t i = 0; i < fEntries.size(); i++) {
			if (fEntries[i].usb == device) {
				_RemoveAt(i);
				return;
			}
		}
	}

	void VolumeMounted(dev_t device)
	{
		BVolume volume(device);
		BVolume boot;
		fVolumeRoster.GetBootVolume(&boot);
		if (volume.InitCheck() != B_OK || volume == boot
			|| !volume.IsPersistent() || volume.IsShared()) {
			return;
		}
		BDirectory root;
		BPath path;
		if (volume.GetRootDirectory(&root) != B_OK
			|| path.SetTo(&root, NULL) != B_OK
			|| !FindDcim(path.Path(), NULL)) {
			return;
		}
		Entry entry;
		entry.device = device;
		entry.path = path.Path();
		entry.info.kind = kCameraVolume;
		char name[B_FILE_NAME_LENGTH];
		if (volume.GetName(name) == B_OK && name[0] != '\0')
			entry.info.name = name;
		else
			entry.info.name = "Memory card";
		char size[32];
		snprintf(size, sizeof(size), ":%" B_PRIdOFF, volume.Capacity());
		entry.info.key = "volume:" + entry.info.name + size;
		_Add(entry);
	}

	void VolumeUnmounted(dev_t device)
	{
		BAutolock locker(fLock);
		for (size_t i = 0; i < fEntries.size(); i++) {
			if (fEntries[i].usb == NULL && fEntries[i].device == device) {
				_RemoveAt(i);
				return;
			}
		}
	}

	std::vector<CameraInfo> Cameras()
	{
		BAutolock locker(fLock);
		std::vector<CameraInfo> cameras;
		for (const Entry& entry : fEntries)
			cameras.push_back(entry.info);
		return cameras;
	}

	CameraSource* CreateSource(const std::string& key)
	{
		BAutolock locker(fLock);
		for (const Entry& entry : fEntries) {
			if (entry.info.key != key)
				continue;
			if (entry.info.kind == kCameraPtp) {
				return new(std::nothrow) PtpCameraSource(entry.info,
					entry.path);
			}
			return new(std::nothrow) VolumeCameraSource(entry.info,
				entry.path);
		}
		return NULL;
	}

private:
	struct Entry {
		CameraInfo		info;
		std::string		path;		// the USB device node, or the volume's root
		BUSBDevice*		usb = NULL;	// the roster's object, to know it again
		dev_t			device = -1;
	};

	class VolumeLooper : public BLooper {
	public:
		VolumeLooper(Impl* owner)
			:
			BLooper("camera volumes"),
			fOwner(owner)
		{
		}

		virtual void MessageReceived(BMessage* message)
		{
			if (message->what != B_NODE_MONITOR) {
				BLooper::MessageReceived(message);
				return;
			}
			int32 opcode = message->GetInt32("opcode", 0);
			if (opcode == B_DEVICE_MOUNTED) {
				dev_t device;
				if (message->FindInt32("new device", &device) == B_OK)
					fOwner->VolumeMounted(device);
			} else if (opcode == B_DEVICE_UNMOUNTED) {
				dev_t device;
				if (message->FindInt32("device", &device) == B_OK)
					fOwner->VolumeUnmounted(device);
			}
		}

	private:
		Impl*	fOwner;
	};

	void _Add(Entry& entry)
	{
		BAutolock locker(fLock);
		// Two of a kind without serial numbers.
		std::string key = entry.info.key;
		for (int n = 2; _Has(entry.info.key); n++) {
			char suffix[16];
			snprintf(suffix, sizeof(suffix), "#%d", n);
			entry.info.key = key + suffix;
		}
		fEntries.push_back(entry);
		_Notify(kMsgCameraArrived, entry.info, true);
	}

	bool _Has(const std::string& key) const
	{
		for (const Entry& entry : fEntries) {
			if (entry.info.key == key)
				return true;
		}
		return false;
	}

	void _RemoveAt(size_t index)
	{
		CameraInfo info = fEntries[index].info;
		fEntries.erase(fEntries.begin() + index);
		_Notify(kMsgCameraRemoved, info, false);
	}

	void _Notify(uint32 what, const CameraInfo& info, bool arrived)
	{
		if (fStopping || !fTarget.IsValid())
			return;
		BMessage message(what);
		message.AddString("key", info.key.c_str());
		message.AddString("name", info.name.c_str());
		message.AddInt32("kind", (int32)info.kind);
		if (arrived)
			message.AddBool("initial", fInitial);
		fTarget.SendMessage(&message, (BHandler*)NULL, 1000000);
	}

	BMessenger			fTarget;
	BLocker				fLock;
	std::vector<Entry>	fEntries;
	BVolumeRoster		fVolumeRoster;
	VolumeLooper*		fLooper;
	bool				fStarted;
	bool				fInitial;
	bool				fStopping;
};


CameraWatcher::CameraWatcher(const BMessenger& target)
	:
	fImpl(new Impl(target))
{
}


CameraWatcher::~CameraWatcher()
{
	delete fImpl;
}


status_t
CameraWatcher::Start()
{
	return fImpl->StartWatching();
}


void
CameraWatcher::Stop()
{
	fImpl->StopWatching();
}


std::vector<CameraInfo>
CameraWatcher::Cameras()
{
	return fImpl->Cameras();
}


CameraSource*
CameraWatcher::CreateSource(const std::string& key)
{
	return fImpl->CreateSource(key);
}

}	// namespace photos
