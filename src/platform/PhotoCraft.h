// The round trip to PhotoCraft (and to any editor): a photo is handed over
// as a working file in the library's edits/<photo>/ folder, the folder is
// watched, and every save there becomes a new version of the photo.
//
//   JPEG and PNG pictures go as they are; anything else (RAW, HEIF, TIFF)
//     as a PNG of the developed picture.
//   A save of a flat picture (JPEG, PNG, TIFF, WebP) is the new version.
//   A save of a layered document (.psd, .pcraft) is kept as the document to
//     open next time, and its composite is the new version.
#pragma once

#include "core/Library.h"
#include "platform/Pictures.h"

#include <Looper.h>
#include <Messenger.h>

#include <map>
#include <string>

class BMessageRunner;

namespace photos {

extern const char* const kPhotoCraftSignature;
extern const char* const kLightCraftSignature;

class PhotoCraftBridge : public BLooper {
public:
	PhotoCraftBridge(Library& library, Pictures& pictures, SyncEngine& sync);
	virtual ~PhotoCraftBridge();

	void SetTarget(const BMessenger& target);
	// Watches the working folders that exist and picks up what was saved
	// while Photos was not running.
	void Start();

	// Prepares the working file and opens it in the application with the
	// signature (PhotoCraft). Blocking: call from a worker thread for RAW
	// files, which are developed first.
	status_t OpenInEditor(int64_t photoId, const char* signature, std::string* error);
	// The file that would be handed over (made if needed).
	status_t PrepareWorkingFile(int64_t photoId, std::string& path, std::string* error);
	// Looks at a photo's working folder now (tests; normally the node
	// monitor does). Returns the number of versions added.
	int CheckNow(int64_t photoId);

	virtual void MessageReceived(BMessage* message);

	static bool IsInstalled(const char* signature);

private:
	struct FileState {
		int64_t size = 0;
		int64_t modified = 0;
		std::string sha1;
	};
	struct Folder {
		node_ref directory;
		std::map<std::string, FileState> files;		// what Photos has seen
		std::map<std::string, FileState> unstable;	// changing right now
	};

	void _Watch(int64_t photoId);
	int _Check(int64_t photoId, bool* again);
	bool _AddVersion(int64_t photoId, const std::string& file, std::string& note);
	void _LoadState(int64_t photoId, Folder& folder);
	void _SaveState(int64_t photoId, const Folder& folder);
	void _Schedule();

	Library& fLibrary;
	Pictures& fPictures;
	SyncEngine& fSync;
	BMessenger fTarget;
	std::map<int64_t, Folder> fFolders;
	BMessageRunner* fRunner;
};

// The flattened picture inside a .pcraft document (a ZIP whose entries are
// stored): the full-size composite when the document has one, else the
// preview. False when there is none.
bool ExtractPcraftComposite(const std::string& path, std::string& png, bool& fullSize);

}	// namespace photos
