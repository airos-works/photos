// The parts of the Immich API Photos uses (tested with server 3.4): login,
// the sync stream of assets, thumbnails and originals, uploads.
#pragma once

#include "core/Http.h"
#include "core/Library.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace photos {

struct SyncEvent {
	enum Kind {
		kAsset,			// an asset is new or changed: `asset`
		kAssetDelete,	// an asset is gone: `assetId`
		kExif,			// an asset's details: `exif`
		kReset,			// the server asks for a sync from the start
		kComplete,		// the end of what there is for now
		kOther
	};
	Kind kind = kOther;
	std::string type;	// the server's name of the entity, e.g. "AssetV2"
	std::string ack;
	RemoteAsset asset;
	bool hidden = false;	// kAsset: not part of the server's timeline
	RemoteExif exif;
	std::string assetId;
};

struct UploadCheck {
	std::string key;		// ours, echoed back
	std::string checksum;	// base64 SHA-1
	bool accept = false;
	bool unsupported = false;
	std::string existingId;	// when the server has the file already
	bool existingTrashed = false;
};

class ImmichClient {
public:
	explicit ImmichClient(Http& http);

	// "http://host:2283" (a trailing "/" or "/api" is dropped).
	void SetServer(const std::string& url);
	const std::string& Server() const { return fServer; }
	// A session token from Login(), or an API key made in Immich's settings.
	void SetToken(const std::string& token) { fToken = token; }
	void SetApiKey(const std::string& key) { fApiKey = key; }
	const std::string& Token() const { return fToken; }
	bool HasCredentials() const { return !fToken.empty() || !fApiKey.empty(); }

	bool Login(const std::string& email, const std::string& password,
		std::string& error);
	void Logout();
	// Checks server and credentials; fills the user's name and the version.
	bool Validate(std::string& error);
	const std::string& UserName() const { return fUserName; }
	const std::string& UserId() const { return fUserId; }
	const std::string& Version() const { return fVersion; }
	int MajorVersion() const { return fMajor; }

	// What changed since the last acknowledged event of this session. The
	// handler returns false to stop.
	typedef std::function<bool(const SyncEvent& event)> SyncHandler;
	bool SyncStream(bool reset, const SyncHandler& handler, std::string& error,
		const std::atomic<bool>* cancelled = nullptr);
	bool Acknowledge(const std::vector<std::string>& acks, std::string& error);

	// size: "thumbnail" (WebP, about 480 pixels) or "preview" (JPEG, 1440).
	bool DownloadThumbnail(const std::string& assetId, const char* size,
		const std::string& path, std::string& error,
		const std::atomic<bool>* cancelled = nullptr);
	bool DownloadOriginal(const std::string& assetId, const std::string& path,
		std::string& error, const std::atomic<bool>* cancelled = nullptr);

	bool CheckUploads(std::vector<UploadCheck>& checks, std::string& error);
	// created/modified: UTC seconds. On success assetId is the new asset, or
	// the one the server had already (duplicate).
	bool Upload(const std::string& path, const std::string& name,
		const std::string& checksum, int64_t created, int64_t modified,
		bool favorite, std::string& assetId, bool& duplicate, std::string& error,
		const std::atomic<bool>* cancelled = nullptr);
	bool SetFavorite(const std::vector<std::string>& assetIds, bool favorite,
		std::string& error);
	// Moves assets to the server's trash (not a permanent delete).
	bool TrashAssets(const std::vector<std::string>& assetIds, std::string& error);

	// True after a reply said the credentials are not accepted (any more).
	bool Unauthorized() const { return fUnauthorized; }

private:
	HttpRequest _Request(const std::string& method, const std::string& path) const;
	bool _Check(const HttpResponse& response, std::string& error);

	Http& fHttp;
	std::string fServer;
	std::string fToken;
	std::string fApiKey;
	std::string fUserName;
	std::string fUserId;
	std::string fVersion;
	int fMajor;
	bool fUnauthorized;
};

// One line of the sync stream as an event; false for lines that are not JSON.
bool ParseSyncLine(const std::string& line, SyncEvent& event);
// "1/250" or "0.004" as seconds.
double ParseExposure(const std::string& text);

}	// namespace photos
