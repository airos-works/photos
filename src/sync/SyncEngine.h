// Two-way sync with an Immich server.
//
//   server -> library: the sync stream tells what is new, changed or gone
//     since the last pass. A photo that is only on the server is listed in
//     the library with its thumbnail; the original comes when it is opened
//     (or for all photos, with keepOriginals).
//   library -> server: photos that are not on the server are uploaded (the
//     server is asked first, by checksum, whether it has them); favourites
//     set here are sent; photos deleted here for good go to the server's
//     trash when trashOnServer is set.
#pragma once

#include "Immich.h"

#include <atomic>
#include <functional>
#include <string>

namespace photos {

struct SyncOptions {
	std::string server;
	std::string token;			// session token (Login) ...
	std::string apiKey;			// ... or an API key
	bool upload = true;
	bool keepOriginals = false;
	bool trashOnServer = false;
};

struct SyncProgress {
	enum Phase { kConnecting, kReceiving, kSending, kUploading, kDownloading, kDone };
	Phase phase = kConnecting;
	int done = 0;
	int total = 0;
};

struct SyncResult {
	bool ok = false;
	bool cancelled = false;
	bool unauthorized = false;
	std::string error;
	int added = 0;			// photos new from the server
	int removed = 0;		// gone on the server
	int uploaded = 0;
	int linked = 0;			// here and there already: now known as the same
	int downloaded = 0;		// originals fetched
	int failed = 0;			// single photos that could not be sent or fetched
	std::string firstFailure;
};

class SyncEngine {
public:
	SyncEngine(Library& library, Http& http);

	void SetOptions(const SyncOptions& options);
	SyncOptions Options();
	bool IsConfigured();

	typedef std::function<void(const SyncProgress& progress)> ProgressFn;
	// One pass. Blocks; one pass at a time (a second call returns at once
	// with an error).
	SyncResult Run(const std::atomic<bool>* cancelled, const ProgressFn& progress);

	// Single files, for whoever shows a photo that is only on the server.
	// May be called from any thread at any time.
	bool FetchThumbnail(const std::string& immichId, const std::string& path,
		std::string& error, const std::atomic<bool>* cancelled = nullptr);
	bool FetchPreview(const std::string& immichId, const std::string& path,
		std::string& error, const std::atomic<bool>* cancelled = nullptr);
	// Downloads the original into the library.
	bool FetchOriginal(int64_t photoId, std::string& error,
		const std::atomic<bool>* cancelled = nullptr);

private:
	bool _Pull(ImmichClient& client, SyncResult& result,
		const std::atomic<bool>* cancelled, const ProgressFn& progress);
	bool _PushChanges(ImmichClient& client, const SyncOptions& options,
		SyncResult& result);
	bool _Upload(ImmichClient& client, SyncResult& result,
		const std::atomic<bool>* cancelled, const ProgressFn& progress);
	void _DownloadOriginals(SyncResult& result,
		const std::atomic<bool>* cancelled, const ProgressFn& progress);
	void _Configure(ImmichClient& client, const SyncOptions& options);

	Library& fLibrary;
	Http& fHttp;
	std::mutex fLock;
	SyncOptions fOptions;
	std::atomic<bool> fRunning;
};

}	// namespace photos
