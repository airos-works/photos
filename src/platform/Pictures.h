// Pictures as the windows need them: square tiles for the grids (cached in
// memory, made by a pool of threads from thumbnail files that are made or
// fetched on demand), the picture of the viewer, and whole decoded pictures
// for the editor and the face scanner.
#pragma once

#include "core/Image.h"
#include "core/Library.h"
#include "sync/SyncEngine.h"

#include <Messenger.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class BBitmap;

namespace photos {

enum {
	// A tile is ready: "id" (int64). For a face tile: "face" (int64).
	kMsgTileReady = 'tilR',
	// The viewer's picture: "token" (int32), "bitmap" (pointer, the receiver
	// owns the BBitmap), "final" (bool: false for a quick stand-in that a
	// better one follows), "full" (bool: the picture in its full size),
	// "remote" (bool: only the server's preview, the original is not here),
	// "error" (string) instead of the bitmap when nothing could be shown.
	kMsgViewPicture = 'viwP'
};

// The longer edge of thumbnail files.
const int kThumbEdge = 512;

class Pictures {
public:
	Pictures(Library& library, SyncEngine& sync);
	~Pictures();

	void SetTarget(const BMessenger& target);
	void SetRawPreviewOnly(bool previewOnly) { fRawPreviewOnly = previewOnly; }

	// The square tile of a photo, `size` pixels wide; null while it is being
	// made (kMsgTileReady follows) or when there is nothing to show.
	std::shared_ptr<BBitmap> Tile(int64_t id, int64_t revision, int size);
	// A face cut out of its photo, square.
	std::shared_ptr<BBitmap> FaceTile(int64_t faceId, int size);
	// True when making the tile failed (a damaged file): draw a placeholder.
	bool TileFailed(int64_t id, int64_t revision);
	// Forgets queued tiles nobody waits for (the grid changed).
	void ForgetQueued();
	// A photo's look changed: its tiles are dropped.
	void Invalidate(int64_t id);

	// The thumbnail file of a photo, made or fetched if needed. Blocking.
	std::string EnsureThumb(const Photo& photo, std::string* error = nullptr);
	// Fetches thumbnails of photos that are only on the server, in the
	// background, a few at a time while nothing else is asked for.
	void PrefetchRemoteThumbs();

	// The picture of a photo for the viewer, at most maxEdge on the longer
	// side (0: full size). Answers with kMsgViewPicture to `target`; a newer
	// request replaces one that has not started.
	void RequestView(int64_t photoId, int maxEdge, const BMessenger& target,
		int32 token);

	// A photo decoded (current version, upright). Blocking.
	//   maxEdge 0: full size.
	//   quick: accept what is fast to get (a RAW file's embedded preview,
	//     the server's preview for a photo whose original is not here).
	//   needOriginal: fetch the original from the server if it is not here.
	status_t Load(const Photo& photo, Image& image, int maxEdge, bool quick,
		bool needOriginal, std::string* error = nullptr, bool* remotePreview = nullptr);
	// The picture a version is based on (the original, or another version).
	status_t LoadBase(const Photo& photo, int64_t baseVersion, Image& image,
		int maxEdge, std::string* error = nullptr);

	size_t CacheBytes();

private:
	struct TileKey {
		int64_t id;			// photo id, or -(face id)
		int64_t revision;
		int size;
		bool operator<(const TileKey& other) const
		{
			if (id != other.id)
				return id < other.id;
			if (revision != other.revision)
				return revision < other.revision;
			return size < other.size;
		}
	};
	struct TileEntry {
		std::shared_ptr<BBitmap> bitmap;
		size_t bytes = 0;
		std::list<TileKey>::iterator position;
	};
	struct ViewRequest {
		int64_t photoId = 0;
		int maxEdge = 0;
		BMessenger target;
		int32 token = 0;
		bool valid = false;
	};

	void _Worker();
	void _ViewWorker();
	void _MakeTile(const TileKey& key);
	bool _LoadThumb(const Photo& photo, Image& image);
	void _Store(const TileKey& key, BBitmap* bitmap);
	void _SendView(const ViewRequest& request, const Image& image, bool final,
		bool full, bool remote);
	void _Trim();

	Library& fLibrary;
	SyncEngine& fSync;
	BMessenger fTarget;
	std::atomic<bool> fRawPreviewOnly;

	std::mutex fLock;
	std::condition_variable fWake;
	std::map<TileKey, TileEntry> fTiles;
	std::list<TileKey> fRecent;			// most recently used first
	size_t fBytes;
	size_t fBudget;
	std::deque<TileKey> fQueue;			// newest request at the back
	std::set<TileKey> fPending;			// queued or being made
	std::set<TileKey> fFailed;
	std::deque<int64_t> fPrefetch;
	std::vector<std::thread> fWorkers;
	bool fQuit;

	std::mutex fViewLock;
	std::condition_variable fViewWake;
	ViewRequest fViewRequest;
	std::thread fViewThread;
	std::atomic<int32> fViewToken;
};

}	// namespace photos
