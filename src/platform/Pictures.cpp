#include "Pictures.h"

#include "ImageIO.h"

#include <Bitmap.h>
#include <Message.h>

#include <algorithm>

namespace photos {

namespace {

const size_t kTileBudget = 384u << 20;
const size_t kMaxQueued = 3000;
const int kThumbQuality = 85;
const int kDevelopedQuality = 93;

}	// namespace


Pictures::Pictures(Library& library, SyncEngine& sync)
	:
	fLibrary(library),
	fSync(sync),
	fRawPreviewOnly(false),
	fBytes(0),
	fBudget(kTileBudget),
	fQuit(false),
	fViewToken(0)
{
	int count = (int)std::thread::hardware_concurrency() / 2;
	count = std::max(2, std::min(count, 8));
	for (int i = 0; i < count; i++)
		fWorkers.emplace_back([this]() { _Worker(); });
	fViewThread = std::thread([this]() { _ViewWorker(); });
}


Pictures::~Pictures()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = true;
	}
	fWake.notify_all();
	{
		std::lock_guard<std::mutex> lock(fViewLock);
		fViewRequest.valid = false;
	}
	fViewWake.notify_all();
	for (auto& worker : fWorkers)
		worker.join();
	fViewThread.join();
}


void
Pictures::SetTarget(const BMessenger& target)
{
	std::lock_guard<std::mutex> lock(fLock);
	fTarget = target;
}


std::shared_ptr<BBitmap>
Pictures::Tile(int64_t id, int64_t revision, int size)
{
	TileKey key = {id, revision, size};
	std::lock_guard<std::mutex> lock(fLock);
	auto found = fTiles.find(key);
	if (found != fTiles.end()) {
		fRecent.splice(fRecent.begin(), fRecent, found->second.position);
		return found->second.bitmap;
	}
	if (fFailed.count(key) != 0)
		return nullptr;
	if (fPending.insert(key).second) {
		fQueue.push_back(key);
		if (fQueue.size() > kMaxQueued) {
			fPending.erase(fQueue.front());
			fQueue.pop_front();
		}
		fWake.notify_one();
	}
	return nullptr;
}


std::shared_ptr<BBitmap>
Pictures::FaceTile(int64_t faceId, int size)
{
	return Tile(-faceId, 0, size);
}


bool
Pictures::TileFailed(int64_t id, int64_t revision)
{
	std::lock_guard<std::mutex> lock(fLock);
	for (auto& key : fFailed) {
		if (key.id == id && key.revision == revision)
			return true;
	}
	return false;
}


void
Pictures::ForgetQueued()
{
	std::lock_guard<std::mutex> lock(fLock);
	for (auto& key : fQueue)
		fPending.erase(key);
	fQueue.clear();
}


void
Pictures::Invalidate(int64_t id)
{
	std::lock_guard<std::mutex> lock(fLock);
	for (auto it = fTiles.begin(); it != fTiles.end();) {
		if (it->first.id == id) {
			fBytes -= it->second.bytes;
			fRecent.erase(it->second.position);
			it = fTiles.erase(it);
		} else
			++it;
	}
	for (auto it = fFailed.begin(); it != fFailed.end();) {
		if (it->id == id)
			it = fFailed.erase(it);
		else
			++it;
	}
}


size_t
Pictures::CacheBytes()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fBytes;
}


void
Pictures::_Trim()
{
	while (fBytes > fBudget && !fRecent.empty()) {
		auto found = fTiles.find(fRecent.back());
		if (found != fTiles.end()) {
			fBytes -= found->second.bytes;
			fTiles.erase(found);
		}
		fRecent.pop_back();
	}
}


void
Pictures::_Store(const TileKey& key, BBitmap* bitmap)
{
	BMessenger target;
	{
		std::lock_guard<std::mutex> lock(fLock);
		fPending.erase(key);
		if (bitmap == nullptr)
			fFailed.insert(key);
		else {
			TileEntry entry;
			entry.bitmap.reset(bitmap);
			entry.bytes = (size_t)bitmap->BitsLength();
			fRecent.push_front(key);
			entry.position = fRecent.begin();
			auto existing = fTiles.find(key);
			if (existing != fTiles.end()) {
				fBytes -= existing->second.bytes;
				fRecent.erase(existing->second.position);
				fTiles.erase(existing);
			}
			fTiles[key] = entry;
			fBytes += entry.bytes;
			_Trim();
		}
		target = fTarget;
	}
	BMessage message(kMsgTileReady);
	if (key.id < 0)
		message.AddInt64("face", -key.id);
	else
		message.AddInt64("id", key.id);
	target.SendMessage(&message);
}


std::string
Pictures::EnsureThumb(const Photo& photo, std::string* error)
{
	std::string path = fLibrary.ThumbPath(photo.id, photo.revision);
	if (FileExists(path))
		return path;
	std::string source = fLibrary.DisplayPath(photo);
	if (!source.empty()) {
		Image image;
		status_t status = DecodeFile(source, image, kThumbEdge,
			kDecodeExact | kDecodeRawPreview, error);
		if (status != B_OK || image.IsEmpty())
			return std::string();
		if (!WriteJpeg(image, kThumbQuality, path)) {
			if (error != nullptr)
				*error = "cannot write the thumbnail";
			return std::string();
		}
		fLibrary.PruneCaches(photo.id, photo.revision);
		return path;
	}
	if (!photo.immichId.empty()) {
		std::string message;
		if (fSync.FetchThumbnail(photo.immichId, path, message))
			return path;
		if (error != nullptr)
			*error = message;
	}
	return std::string();
}


bool
Pictures::_LoadThumb(const Photo& photo, Image& image)
{
	std::string path = EnsureThumb(photo);
	if (path.empty())
		return false;
	if (DecodeFile(path, image) == B_OK && !image.IsEmpty())
		return true;
	// A damaged thumbnail file: make it again next time.
	RemoveFile(path);
	return false;
}


void
Pictures::_MakeTile(const TileKey& key)
{
	Image tile;
	if (key.id < 0) {
		// A face: the crop saved when the photo was scanned, else cut from
		// the photo's thumbnail.
		Face face;
		if (!fLibrary.GetFace(-key.id, face)) {
			_Store(key, nullptr);
			return;
		}
		Image crop;
		std::string saved = fLibrary.FaceThumbPath(face.id);
		if (FileExists(saved) && DecodeFile(saved, crop) == B_OK && !crop.IsEmpty())
			ScaleImage(crop, key.size, key.size, tile);
		else {
			Photo photo;
			Image thumb;
			if (!fLibrary.Get(face.photoId, photo) || !_LoadThumb(photo, thumb)) {
				_Store(key, nullptr);
				return;
			}
			float x, y, w, h;
			FaceCropRect(face, thumb.width, thumb.height, x, y, w, h);
			CropImage(thumb, x, y, w, h, key.size, key.size, tile);
		}
	} else {
		Photo photo;
		Image thumb;
		if (!fLibrary.Get(key.id, photo) || photo.revision != key.revision
			|| !_LoadThumb(photo, thumb)) {
			_Store(key, nullptr);
			return;
		}
		CoverImage(thumb, key.size, key.size, tile);
	}
	_Store(key, MakeBitmap(tile));
}


void
Pictures::_Worker()
{
	for (;;) {
		TileKey key = {0, 0, 0};
		int64_t prefetch = 0;
		{
			std::unique_lock<std::mutex> lock(fLock);
			fWake.wait(lock, [this]() {
				return fQuit || !fQueue.empty() || !fPrefetch.empty();
			});
			if (fQuit)
				return;
			if (!fQueue.empty()) {
				// The newest request first: it is what the user looks at.
				key = fQueue.back();
				fQueue.pop_back();
			} else {
				prefetch = fPrefetch.front();
				fPrefetch.pop_front();
			}
		}
		if (prefetch != 0) {
			Photo photo;
			if (fLibrary.Get(prefetch, photo))
				EnsureThumb(photo);
		} else
			_MakeTile(key);
	}
}


void
Pictures::PrefetchRemoteThumbs()
{
	std::vector<PhotoBrief> photos;
	fLibrary.ListRemoteOnly(photos);
	std::vector<int64_t> missing;
	for (auto& photo : photos) {
		if (!FileExists(fLibrary.ThumbPath(photo.id, photo.revision)))
			missing.push_back(photo.id);
	}
	{
		std::lock_guard<std::mutex> lock(fLock);
		fPrefetch.assign(missing.begin(), missing.end());
	}
	fWake.notify_all();
}


// --- The viewer's picture ---------------------------------------------------

void
Pictures::RequestView(int64_t photoId, int maxEdge, const BMessenger& target,
	int32 token)
{
	{
		std::lock_guard<std::mutex> lock(fViewLock);
		fViewRequest.photoId = photoId;
		fViewRequest.maxEdge = maxEdge;
		fViewRequest.target = target;
		fViewRequest.token = token;
		fViewRequest.valid = true;
		fViewToken = token;
	}
	fViewWake.notify_one();
}


void
Pictures::_SendView(const ViewRequest& request, const Image& image, bool final,
	bool full, bool remote)
{
	if (request.token != fViewToken)
		return;
	BBitmap* bitmap = MakeBitmap(image);
	BMessage message(kMsgViewPicture);
	message.AddInt32("token", request.token);
	message.AddInt64("id", request.photoId);
	message.AddBool("final", final);
	message.AddBool("full", full);
	message.AddBool("remote", remote);
	if (bitmap != nullptr)
		message.AddPointer("bitmap", bitmap);
	else
		message.AddString("error", "not enough memory for this picture");
	if (request.target.SendMessage(&message) != B_OK)
		delete bitmap;
}


void
Pictures::_ViewWorker()
{
	for (;;) {
		ViewRequest request;
		{
			std::unique_lock<std::mutex> lock(fViewLock);
			fViewWake.wait(lock, [this]() {
				std::lock_guard<std::mutex> quitLock(fLock);
				return fQuit || fViewRequest.valid;
			});
			{
				std::lock_guard<std::mutex> quitLock(fLock);
				if (fQuit)
					return;
			}
			request = fViewRequest;
			fViewRequest.valid = false;
		}
		Photo photo;
		std::string error;
		Image image;
		bool remote = false;
		status_t status = B_ENTRY_NOT_FOUND;
		if (fLibrary.Get(request.photoId, photo)) {
			bool develop = photo.kind == kKindRaw && photo.version == 0
				&& !photo.path.empty() && !fRawPreviewOnly
				&& !FileExists(fLibrary.PreviewPath(photo.id, photo.revision));
			if (develop) {
				// Developing takes a moment: the camera's own preview first.
				Image quick;
				if (Load(photo, quick, request.maxEdge > 0 ? request.maxEdge : 2560,
						true, false) == B_OK)
					_SendView(request, quick, false, false, false);
			}
			if (request.token != fViewToken)
				continue;
			status = Load(photo, image, request.maxEdge, false, false, &error,
				&remote);
		} else
			error = "the photo is gone";
		if (status == B_OK && !image.IsEmpty()) {
			bool full = !remote && (request.maxEdge == 0
				|| std::max(image.width, image.height) < request.maxEdge);
			_SendView(request, image, true, full, remote);
		} else if (request.token == fViewToken) {
			BMessage message(kMsgViewPicture);
			message.AddInt32("token", request.token);
			message.AddInt64("id", request.photoId);
			message.AddBool("final", true);
			message.AddString("error", error.empty()
				? "this picture cannot be shown" : error.c_str());
			request.target.SendMessage(&message);
		}
	}
}


status_t
Pictures::Load(const Photo& photo, Image& image, int maxEdge, bool quick,
	bool needOriginal, std::string* error, bool* remotePreview)
{
	if (remotePreview != nullptr)
		*remotePreview = false;
	Photo current = photo;
	std::string path = fLibrary.DisplayPath(current);
	if (path.empty()) {
		if (current.immichId.empty()) {
			if (error != nullptr)
				*error = "the original is missing";
			return B_ENTRY_NOT_FOUND;
		}
		if (needOriginal) {
			std::string message;
			if (!fSync.FetchOriginal(current.id, message)
				|| !fLibrary.Get(current.id, current)) {
				if (error != nullptr)
					*error = "cannot get the original from Immich: " + message;
				return B_IO_ERROR;
			}
			path = fLibrary.DisplayPath(current);
		} else {
			// The server's screen-size rendering.
			std::string preview = fLibrary.PreviewPath(current.id, current.revision);
			if (!FileExists(preview)) {
				std::string message;
				if (!fSync.FetchPreview(current.immichId, preview, message)) {
					if (error != nullptr)
						*error = "cannot get the picture from Immich: " + message;
					return B_IO_ERROR;
				}
			}
			if (remotePreview != nullptr)
				*remotePreview = true;
			status_t status = DecodeFile(preview, image, maxEdge, kDecodeExact, error);
			if (status != B_OK)
				RemoveFile(preview);
			return status;
		}
	}

	bool raw = KindOfName(path) == kKindRaw;
	if (!raw)
		return DecodeFile(path, image, maxEdge, kDecodeExact, error);
	if (quick || fRawPreviewOnly) {
		return DecodeFile(path, image, maxEdge, kDecodeExact | kDecodeRawPreview,
			error);
	}
	// Developed once, kept as a JPEG next to the thumbnails.
	std::string developed = fLibrary.PreviewPath(current.id, current.revision);
	if (FileExists(developed)
		&& DecodeFile(developed, image, maxEdge, kDecodeExact) == B_OK)
		return B_OK;
	Image full;
	status_t status = DecodeFile(path, full, 0, 0, error);
	if (status != B_OK) {
		// A camera the engine does not know: its preview is better than
		// nothing.
		return DecodeFile(path, image, maxEdge, kDecodeExact | kDecodeRawPreview,
			error);
	}
	WriteJpeg(full, kDevelopedQuality, developed);
	if (maxEdge > 0 && std::max(full.width, full.height) > maxEdge)
		FitImage(full, maxEdge, image);
	else
		image = std::move(full);
	return B_OK;
}


status_t
Pictures::LoadBase(const Photo& photo, int64_t baseVersion, Image& image,
	int maxEdge, std::string* error)
{
	Photo base = photo;
	base.version = baseVersion;
	return Load(base, image, maxEdge, false, true, error);
}

}	// namespace photos
