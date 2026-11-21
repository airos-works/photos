// Messages the background services send to the window (see Pictures.h for
// the picture ones).
#pragma once

#include <SupportDefs.h>

namespace photos {

enum {
	// The library changed: "changes" (int32, LibraryChange bits), "id".
	// Sent once until the receiver calls Session::LibraryChangeSeen().
	kMsgLibraryChanged = 'libC',
	// Import: "done", "total", "added", "duplicates", "failed" (int32),
	// "name" (string), "running" (bool), "import" (int64), "source".
	kMsgImportProgress = 'impP',
	// Sync: "running" (bool), "phase", "done", "total" (int32), "text".
	kMsgSyncStatus = 'synS',
	// Faces: "running" (bool), "done", "total" (int32), "device", "error".
	kMsgFacesProgress = 'facP',
	// A version arrived from PhotoCraft: "id" (photo), "version" (int64),
	// "note" (string, when there is something to tell the user).
	kMsgVersionAdded = 'verA',
	// A people operation finished (naming a group takes a moment).
	kMsgPeopleChanged = 'pplC',
	// Cameras (CameraSource.h): 'camA' / 'camR' with "key", "name", "kind".
	kMsgCameraArrived = 'camA',
	kMsgCameraRemoved = 'camR'
};

}	// namespace photos
