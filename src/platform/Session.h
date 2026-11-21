// Everything of Photos that is not a window: the settings, the library and
// the services around it. The application and photos_tool each make one.
#pragma once

#include "core/Library.h"
#include "core/People.h"
#include "core/Settings.h"
#include "net/CurlHttp.h"
#include "platform/FaceScanner.h"
#include "platform/Importer.h"
#include "platform/PhotoCraft.h"
#include "platform/Pictures.h"
#include "sync/SyncEngine.h"

#include <Messenger.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace photos {

struct SyncState {
	bool configured = false;
	bool running = false;
	int phase = 0;			// SyncProgress::Phase
	int done = 0;
	int total = 0;
	int64_t lastSync = 0;	// UTC
	std::string text;		// one line for the window
	std::string error;
};

class Session {
public:
	Session();
	~Session();

	// Reads the settings and opens (or makes) the library.
	//   PHOTOS_SETTINGS_DIR, PHOTOS_LIBRARY: other places for both (tests).
	bool Open(std::string* error);
	// Starts what runs in the background and reports to `target`.
	void Start(const BMessenger& target, bool withFaces = true);
	void Stop();

	Settings& Prefs() { return fSettings; }
	Library& Lib() { return fLibrary; }
	People& Persons() { return fPeople; }
	Pictures& Pics() { return *fPictures; }
	Importer& Imports() { return *fImporter; }
	FaceScanner& Faces() { return *fFaces; }
	PhotoCraftBridge& Editors() { return *fBridge; }
	SyncEngine& Sync() { return fSync; }

	// What changed in the library since the last call (LibraryChange bits);
	// photoId is the one photo concerned, 0 when several were. A
	// kMsgLibraryChanged is sent when there is something to take.
	uint32_t TakeLibraryChanges(int64_t& photoId);

	// --- Immich ---
	// Signs in (blocking) and keeps the session in the settings.
	bool SignIn(const std::string& server, const std::string& email,
		const std::string& password, std::string& error);
	bool UseApiKey(const std::string& server, const std::string& key,
		std::string& error);
	void SignOut();
	// Settings changed: hand them to the sync engine.
	void ApplySyncSettings();
	// Starts a sync pass now (no-op while one runs).
	void SyncNow();
	void CancelSync();
	SyncState GetSyncState();
	// One pass, blocking (photos_tool).
	SyncResult SyncBlocking();

	// Runs `work` on the session's worker thread (things that take a
	// moment: naming a group of faces, rendering an edit).
	void Async(std::function<void()> work);

	// Where the face models are: next to the application's data.
	std::string ModelDirectory();
	void ApplyFaceSettings();
	void ApplyDeviceSettings();

	std::string SettingsDirectory() const { return fSettingsDirectory; }

private:
	void _SyncLoop();
	void _WorkLoop();
	void _SendSyncState();
	void _SetSyncText(const SyncProgress& progress);

	Settings fSettings;
	Library fLibrary;
	CurlHttp fHttp;
	SyncEngine fSync;
	People fPeople;
	Pictures* fPictures;
	Importer* fImporter;
	FaceScanner* fFaces;
	PhotoCraftBridge* fBridge;
	BMessenger fTarget;
	std::string fSettingsDirectory;
	std::mutex fChangeLock;
	bool fChangePending;
	uint32_t fChangeBits;
	int64_t fChangePhoto;		// 0: none yet, -1: several
	bool fStarted;

	std::mutex fSyncLock;
	std::condition_variable fSyncWake;
	std::thread fSyncThread;
	std::atomic<bool> fSyncCancel;
	bool fSyncRequested;
	bool fSyncQuit;
	SyncState fSyncState;

	std::mutex fWorkLock;
	std::condition_variable fWorkWake;
	std::deque<std::function<void()>> fWork;
	std::thread fWorkThread;
	bool fWorkQuit;
};

}	// namespace photos
