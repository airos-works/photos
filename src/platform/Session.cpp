#include "Session.h"

#include "ImageIO.h"
#include "Messages.h"

#include <Application.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>
#include <Roster.h>

#include <cstdlib>
#include <ctime>

#if __has_include("core/FaceMath.h")
#include "core/FaceMath.h"
#define PHOTOS_HAVE_FACEMATH 1
#endif

namespace photos {

Session::Session()
	:
	fSync(fLibrary, fHttp),
	fPeople(fLibrary),
	fPictures(nullptr),
	fImporter(nullptr),
	fFaces(nullptr),
	fBridge(nullptr),
	fChangePending(false),
	fChangeBits(0),
	fChangePhoto(0),
	fStarted(false),
	fSyncCancel(false),
	fSyncRequested(false),
	fSyncQuit(false),
	fWorkQuit(false)
{
}


Session::~Session()
{
	Stop();
	fLibrary.SetListener(nullptr);
	delete fFaces;
	delete fImporter;
	if (fBridge != nullptr) {
		if (fBridge->Lock())
			fBridge->Quit();
	}
	delete fPictures;
}


bool
Session::Open(std::string* error)
{
	BPath path;
	const char* settingsOverride = getenv("PHOTOS_SETTINGS_DIR");
	if (settingsOverride != nullptr && settingsOverride[0] != '\0')
		fSettingsDirectory = settingsOverride;
	else if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK)
		fSettingsDirectory = JoinPath(path.Path(), "Photos");
	else
		fSettingsDirectory = "/boot/home/config/settings/Photos";
	MakeDirs(fSettingsDirectory);
	fSettings.Load(JoinPath(fSettingsDirectory, "settings.json"));

	std::string library;
	const char* libraryOverride = getenv("PHOTOS_LIBRARY");
	if (libraryOverride != nullptr && libraryOverride[0] != '\0')
		library = libraryOverride;
	else
		library = fSettings.GetString(setting::kLibraryPath);
	if (library.empty()) {
		BPath home;
		if (find_directory(B_USER_DIRECTORY, &home) != B_OK)
			home.SetTo("/boot/home");
		library = JoinPath(home.Path(), "Pictures/Photos Library");
	}
	if (!fLibrary.Open(library, error))
		return false;
	fLibrary.SetMetadataReader(ReadFileMetadata);
	fLibrary.ExpireTrash((int)fSettings.GetInt(setting::kTrashDays, 30));

#ifdef PHOTOS_HAVE_FACEMATH
	PeopleThresholds thresholds;
	thresholds.same = kSameFaceThreshold;
	thresholds.margin = kSameFaceMargin;
	thresholds.certain = kCertainFaceThreshold;
	thresholds.maybe = kMaybeSameFaceThreshold;
	thresholds.minQuality = kFaceMinQuality;
	fPeople.SetThresholds(thresholds);
#endif
	fPeople.Load();

	fPictures = new Pictures(fLibrary, fSync);
	fPictures->SetRawPreviewOnly(fSettings.GetBool(setting::kRawUsePreview, false));
	fImporter = new Importer(fLibrary);
	fFaces = new FaceScanner(fLibrary, fPeople, *fPictures, fSync);
	fBridge = new PhotoCraftBridge(fLibrary, *fPictures, fSync);
	ApplySyncSettings();
	return true;
}


void
Session::Start(const BMessenger& target, bool withFaces)
{
	fTarget = target;
	fStarted = true;
	fLibrary.SetListener([this](uint32_t changes, int64_t photoId) {
		if ((changes & kChangedList) != 0 && fFaces != nullptr)
			fFaces->Wake();
		// One message until the window has caught up: a sync or an import
		// makes thousands of changes.
		bool send;
		{
			std::lock_guard<std::mutex> lock(fChangeLock);
			fChangeBits |= changes;
			if (photoId != 0) {
				if (fChangePhoto == 0)
					fChangePhoto = photoId;
				else if (fChangePhoto != photoId)
					fChangePhoto = -1;
			} else if ((changes & kChangedList) != 0)
				fChangePhoto = -1;
			send = !fChangePending;
			fChangePending = true;
		}
		if (send && fTarget.SendMessage(kMsgLibraryChanged) != B_OK) {
			std::lock_guard<std::mutex> lock(fChangeLock);
			fChangePending = false;
		}
	});
	fPictures->SetTarget(target);
	fImporter->SetTarget(target);
	fFaces->SetTarget(target);
	fBridge->SetTarget(target);
	fBridge->Start();

	fWorkThread = std::thread([this]() { _WorkLoop(); });
	fSyncThread = std::thread([this]() { _SyncLoop(); });
	if (withFaces)
		ApplyFaceSettings();
	ApplyDeviceSettings();
	if (fSync.IsConfigured() && fSettings.GetBool(setting::kImmichEnabled, true))
		SyncNow();
}


uint32_t
Session::TakeLibraryChanges(int64_t& photoId)
{
	std::lock_guard<std::mutex> lock(fChangeLock);
	uint32_t bits = fChangeBits;
	photoId = fChangePhoto > 0 ? fChangePhoto : 0;
	fChangeBits = 0;
	fChangePhoto = 0;
	fChangePending = false;
	return bits;
}


void
Session::Stop()
{
	if (!fStarted)
		return;
	fStarted = false;
	fSyncCancel = true;
	{
		std::lock_guard<std::mutex> lock(fSyncLock);
		fSyncQuit = true;
	}
	fSyncWake.notify_all();
	{
		std::lock_guard<std::mutex> lock(fWorkLock);
		fWorkQuit = true;
	}
	fWorkWake.notify_all();
	if (fSyncThread.joinable())
		fSyncThread.join();
	if (fWorkThread.joinable())
		fWorkThread.join();
	if (fFaces != nullptr)
		fFaces->Stop();
	if (fImporter != nullptr)
		fImporter->Cancel();
	fSettings.Save();
}


void
Session::Async(std::function<void()> work)
{
	{
		std::lock_guard<std::mutex> lock(fWorkLock);
		fWork.push_back(std::move(work));
	}
	fWorkWake.notify_one();
}


void
Session::_WorkLoop()
{
	for (;;) {
		std::function<void()> work;
		{
			std::unique_lock<std::mutex> lock(fWorkLock);
			fWorkWake.wait(lock, [this]() { return fWorkQuit || !fWork.empty(); });
			if (fWorkQuit)
				return;
			work = std::move(fWork.front());
			fWork.pop_front();
		}
		work();
	}
}


// --- Immich -----------------------------------------------------------------

void
Session::ApplySyncSettings()
{
	SyncOptions options;
	if (fSettings.GetBool(setting::kImmichEnabled, true)) {
		options.server = fSettings.GetString(setting::kImmichServer);
		options.token = fSettings.GetString(setting::kImmichToken);
		options.apiKey = fSettings.GetString(setting::kImmichApiKey);
	}
	options.upload = fSettings.GetBool(setting::kImmichUpload, true);
	options.keepOriginals = fSettings.GetBool(setting::kImmichKeepOriginals, false);
	options.trashOnServer = fSettings.GetBool(setting::kImmichTrashOnServer, false);
	fSync.SetOptions(options);
	{
		std::lock_guard<std::mutex> lock(fSyncLock);
		fSyncState.configured = fSync.IsConfigured();
		if (!fSyncState.configured) {
			fSyncState.text.clear();
			fSyncState.error.clear();
		}
	}
	if (fPictures != nullptr)
		fPictures->SetRawPreviewOnly(fSettings.GetBool(setting::kRawUsePreview, false));
}


bool
Session::SignIn(const std::string& server, const std::string& email,
	const std::string& password, std::string& error)
{
	ImmichClient client(fHttp);
	client.SetServer(server);
	if (client.Server().empty()) {
		error = "Enter the server's address, for example http://192.168.1.10:2283";
		return false;
	}
	if (!client.Login(email, password, error))
		return false;
	if (!client.Validate(error))
		return false;
	fSettings.SetString(setting::kImmichServer, client.Server());
	fSettings.SetString(setting::kImmichUser, email);
	fSettings.SetString(setting::kImmichToken, client.Token());
	fSettings.Remove(setting::kImmichApiKey);
	fSettings.SetBool(setting::kImmichEnabled, true);
	fSettings.Save();
	ApplySyncSettings();
	return true;
}


bool
Session::UseApiKey(const std::string& server, const std::string& key,
	std::string& error)
{
	ImmichClient client(fHttp);
	client.SetServer(server);
	client.SetApiKey(key);
	if (!client.Validate(error))
		return false;
	fSettings.SetString(setting::kImmichServer, client.Server());
	fSettings.SetString(setting::kImmichUser, client.UserName());
	fSettings.SetString(setting::kImmichApiKey, key);
	fSettings.Remove(setting::kImmichToken);
	fSettings.SetBool(setting::kImmichEnabled, true);
	fSettings.Save();
	ApplySyncSettings();
	return true;
}


void
Session::SignOut()
{
	CancelSync();
	std::string token = fSettings.GetString(setting::kImmichToken);
	if (!token.empty()) {
		ImmichClient client(fHttp);
		client.SetServer(fSettings.GetString(setting::kImmichServer));
		client.SetToken(token);
		client.Logout();
	}
	fSettings.Remove(setting::kImmichToken);
	fSettings.Remove(setting::kImmichApiKey);
	fSettings.Save();
	ApplySyncSettings();
	_SendSyncState();
}


void
Session::SyncNow()
{
	{
		std::lock_guard<std::mutex> lock(fSyncLock);
		fSyncRequested = true;
	}
	fSyncWake.notify_all();
}


void
Session::CancelSync()
{
	fSyncCancel = true;
}


SyncState
Session::GetSyncState()
{
	std::lock_guard<std::mutex> lock(fSyncLock);
	SyncState state = fSyncState;
	state.configured = fSync.IsConfigured();
	return state;
}


void
Session::_SendSyncState()
{
	SyncState state = GetSyncState();
	BMessage message(kMsgSyncStatus);
	message.AddBool("configured", state.configured);
	message.AddBool("running", state.running);
	message.AddInt32("phase", state.phase);
	message.AddInt32("done", state.done);
	message.AddInt32("total", state.total);
	message.AddString("text", state.text.c_str());
	if (!state.error.empty())
		message.AddString("error", state.error.c_str());
	fTarget.SendMessage(&message);
}


void
Session::_SetSyncText(const SyncProgress& progress)
{
	std::string text;
	switch (progress.phase) {
		case SyncProgress::kConnecting:
			text = "Connecting to Immich…";
			break;
		case SyncProgress::kReceiving:
			text = "Receiving changes…";
			if (progress.done > 0)
				text = "Receiving changes (" + std::to_string(progress.done) + ")…";
			break;
		case SyncProgress::kSending:
			text = "Sending changes…";
			break;
		case SyncProgress::kUploading:
			text = "Uploading " + std::to_string(std::min(progress.done + 1,
				progress.total)) + " of " + std::to_string(progress.total) + "…";
			if (progress.total == 0)
				text = "Checking for photos to upload…";
			break;
		case SyncProgress::kDownloading:
			text = "Downloading originals: " + std::to_string(progress.done + 1)
				+ " of " + std::to_string(progress.total) + "…";
			break;
		case SyncProgress::kDone:
			return;
	}
	{
		std::lock_guard<std::mutex> lock(fSyncLock);
		fSyncState.phase = progress.phase;
		fSyncState.done = progress.done;
		fSyncState.total = progress.total;
		fSyncState.text = text;
	}
	_SendSyncState();
}


SyncResult
Session::SyncBlocking()
{
	std::atomic<bool> cancelled(false);
	return fSync.Run(&cancelled, nullptr);
}


void
Session::_SyncLoop()
{
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(fSyncLock);
			int64_t minutes = std::max<int64_t>(1,
				fSettings.GetInt(setting::kImmichInterval, 15));
			fSyncWake.wait_for(lock, std::chrono::minutes(minutes),
				[this]() { return fSyncQuit || fSyncRequested; });
			if (fSyncQuit)
				return;
			fSyncRequested = false;
			if (!fSync.IsConfigured()
				|| !fSettings.GetBool(setting::kImmichEnabled, true))
				continue;
			fSyncState.running = true;
			fSyncState.error.clear();
		}
		fSyncCancel = false;
		bigtime_t lastReport = 0;
		SyncResult result = fSync.Run(&fSyncCancel,
			[&](const SyncProgress& progress) {
				bigtime_t now = system_time();
				if (now - lastReport < 200000
					&& progress.phase != SyncProgress::kConnecting
					&& progress.phase != SyncProgress::kSending)
					return;
				lastReport = now;
				_SetSyncText(progress);
			});
		{
			std::lock_guard<std::mutex> lock(fSyncLock);
			fSyncState.running = false;
			fSyncState.done = fSyncState.total = 0;
			if (result.ok) {
				fSyncState.lastSync = NowSeconds();
				time_t now = time(nullptr);
				struct tm local;
				localtime_r(&now, &local);
				char clock[16];
				strftime(clock, sizeof(clock), "%H:%M", &local);
				fSyncState.text = std::string("Up to date · ") + clock;
				if (result.failed > 0) {
					fSyncState.error = std::to_string(result.failed)
						+ (result.failed == 1 ? " photo" : " photos")
						+ " could not be transferred: " + result.firstFailure;
				}
			} else if (result.cancelled)
				fSyncState.text = "Sync stopped";
			else {
				fSyncState.text = "Sync failed";
				fSyncState.error = result.unauthorized
					? "Immich no longer accepts the sign-in. Sign in again in "
						"the settings."
					: result.error;
			}
		}
		_SendSyncState();
		if (result.ok) {
			if (fPictures != nullptr)
				fPictures->PrefetchRemoteThumbs();
			if (fFaces != nullptr)
				fFaces->Wake();
		}
	}
}


// --- Faces, devices ---------------------------------------------------------

std::string
Session::ModelDirectory()
{
	const char* override = getenv("PHOTOS_MODELS");
	if (override != nullptr && override[0] != '\0')
		return override;
	// Next to the program (a build tree), then the data directories.
	app_info info;
	if (be_app != nullptr && be_app->GetAppInfo(&info) == B_OK) {
		BPath path(&info.ref);
		BPath parent;
		if (path.GetParent(&parent) == B_OK) {
			std::string local = JoinPath(parent.Path(), "models");
			if (FileExists(local))
				return local;
		}
	}
	static const directory_which kWhich[] = {
		B_USER_NONPACKAGED_DATA_DIRECTORY, B_USER_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY, B_SYSTEM_DATA_DIRECTORY
	};
	for (directory_which which : kWhich) {
		BPath path;
		if (find_directory(which, &path) != B_OK)
			continue;
		std::string candidate = JoinPath(path.Path(), "Photos/models");
		if (FileExists(candidate))
			return candidate;
	}
	return "/boot/system/data/Photos/models";
}


void
Session::ApplyFaceSettings()
{
	if (fFaces == nullptr)
		return;
	if (fSettings.GetBool(setting::kFacesEnabled, true)) {
		fFaces->Start(ModelDirectory(),
			fSettings.GetBool(setting::kFacesCpuOnly, false));
	} else
		fFaces->Stop();
}


void
Session::ApplyDeviceSettings()
{
	// photos_agent reads this small file.
	std::string text = std::string("open_on_connect ")
		+ (fSettings.GetBool(setting::kOpenOnConnect, true) ? "true" : "false") + "\n";
	WriteFile(JoinPath(fSettingsDirectory, "agent"), text.data(), text.size());
}

}	// namespace photos
