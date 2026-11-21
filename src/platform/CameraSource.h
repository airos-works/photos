// Cameras, phones and memory cards as sources of pictures: a device that
// speaks PTP or MTP over USB, or a mounted volume with a DCIM folder.
// CameraWatcher tells when one arrives or goes; a CameraSource lists what
// is on it and fetches it.
#pragma once

#include <Messenger.h>
#include <SupportDefs.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace photos {

struct CameraItem {
	std::string id;			// stable within the device: PTP object handle (hex) or path relative to the volume root
	std::string name;		// file name
	std::string folder;		// folder on the device, for display
	int64_t size = 0;
	int64_t taken = 0;		// capture (else modification) time, seconds since the epoch; 0 unknown
	bool isVideo = false;
	bool isRaw = false;
};

enum CameraKind { kCameraPtp, kCameraVolume };

struct CameraInfo {
	std::string key;		// stable identity: "usb:<vid>:<pid>:<serial>" / "volume:<name>:<size>"
	std::string name;		// "Canon EOS R6", "iPhone", the volume's name
	CameraKind kind = kCameraPtp;
};

// A source is used by one thread at a time or by several: its calls are
// serialized inside (a thumbnail asked for during a download waits for the
// download). All calls block on the device; none belongs in a window thread.
class CameraSource {
public:
	virtual						~CameraSource() {}

	virtual	const CameraInfo&	Info() const = 0;
	// A locked phone fails here (B_PERMISSION_DENIED) with words for the
	// user; a camera without a card with B_DEV_NO_MEDIA; a device that was
	// unplugged with B_DEV_NOT_READY. After a failed Open() the source can
	// be opened again (once the phone is unlocked).
	virtual	status_t			Open(std::string* error) = 0;
	// Pictures and videos (flagged), in the device's order. B_CANCELED when
	// *cancel became true. progress is called from the calling thread.
	virtual	status_t			ListItems(std::vector<CameraItem>& items,
									const std::atomic<bool>* cancel,
									const std::function<void(int32 found)>& progress) = 0;
	// A small JPEG: the device's thumbnail or the one in the file's EXIF
	// block. B_UNSUPPORTED if there is none.
	virtual	status_t			GetThumbnail(const CameraItem& item,
									std::vector<uint8_t>& jpeg) = 0;
	// Writes the item to path (the file is replaced; its modification time
	// becomes the item's). progress returns false to cancel (B_CANCELED);
	// a partial file is removed.
	virtual	status_t			Download(const CameraItem& item,
									const char* path,
									const std::function<bool(int64 done, int64 total)>& progress) = 0;
	virtual	status_t			DeleteItem(const CameraItem& item) = 0;
	virtual	void				Close() = 0;

	// Words for the status of the call before, for a person; empty if it
	// succeeded.
	virtual	std::string			LastError() const = 0;
};


enum {
	kMsgCameraArrived = 'camA',
	kMsgCameraRemoved = 'camR'
};

// Watches USB (BUSBRoster) and volumes (BVolumeRoster): sends
// kMsgCameraArrived / kMsgCameraRemoved to the target, with
//   "key" (string), "name" (string), "kind" (int32, a CameraKind) and, on
//   arrival, "initial" (bool): it was there already when Start() looked.
class CameraWatcher {
public:
								CameraWatcher(const BMessenger& target);
								~CameraWatcher();

	// Also reports what is connected already ("initial" true), before it
	// returns.
			status_t			Start();
			void				Stop();
			std::vector<CameraInfo> Cameras();
	// The caller owns the source; NULL if the camera is gone.
			CameraSource*		CreateSource(const std::string& key);

private:
	class Impl;
			Impl*				fImpl;
};

}	// namespace photos
