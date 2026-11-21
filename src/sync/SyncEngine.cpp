#include "SyncEngine.h"

#include <map>
#include <set>

namespace photos {

namespace {

bool
Cancelled(const std::atomic<bool>* cancelled)
{
	return cancelled != nullptr && *cancelled;
}

}	// namespace


SyncEngine::SyncEngine(Library& library, Http& http)
	:
	fLibrary(library),
	fHttp(http),
	fRunning(false)
{
}


void
SyncEngine::SetOptions(const SyncOptions& options)
{
	std::lock_guard<std::mutex> lock(fLock);
	fOptions = options;
}


SyncOptions
SyncEngine::Options()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fOptions;
}


bool
SyncEngine::IsConfigured()
{
	std::lock_guard<std::mutex> lock(fLock);
	return !fOptions.server.empty()
		&& (!fOptions.token.empty() || !fOptions.apiKey.empty());
}


void
SyncEngine::_Configure(ImmichClient& client, const SyncOptions& options)
{
	client.SetServer(options.server);
	client.SetToken(options.token);
	client.SetApiKey(options.apiKey);
}


SyncResult
SyncEngine::Run(const std::atomic<bool>* cancelled, const ProgressFn& progress)
{
	SyncResult result;
	if (!IsConfigured()) {
		result.error = "no Immich server is set up";
		return result;
	}
	bool expected = false;
	if (!fRunning.compare_exchange_strong(expected, true)) {
		result.error = "a sync is running already";
		return result;
	}
	SyncOptions options = Options();
	ImmichClient client(fHttp);
	_Configure(client, options);

	auto report = [&](SyncProgress::Phase phase, int done, int total) {
		if (progress) {
			SyncProgress state;
			state.phase = phase;
			state.done = done;
			state.total = total;
			progress(state);
		}
	};
	report(SyncProgress::kConnecting, 0, 0);

	bool ok = client.Validate(result.error);
	// The library belongs to one account on one server: another one starts
	// from nothing.
	if (ok) {
		std::string identity = client.Server() + "|" + client.UserId();
		std::string known = fLibrary.Setting("immich.identity");
		if (known != identity) {
			if (!known.empty())
				fLibrary.ForgetImmich();
			fLibrary.SetSetting("immich.identity", identity);
			fLibrary.SetSetting("immich.initial", "");
		}
	}
	ok = ok && _Pull(client, result, cancelled, progress);
	if (ok && !Cancelled(cancelled)) {
		report(SyncProgress::kSending, 0, 0);
		ok = _PushChanges(client, options, result);
	}
	if (ok && options.upload && !Cancelled(cancelled))
		ok = _Upload(client, result, cancelled, progress);
	if (ok && options.keepOriginals && !Cancelled(cancelled))
		_DownloadOriginals(result, cancelled, progress);

	result.unauthorized = client.Unauthorized();
	result.cancelled = Cancelled(cancelled);
	result.ok = ok && !result.cancelled;
	if (result.ok)
		fLibrary.SetSetting("immich.last_sync", std::to_string(NowSeconds()));
	report(SyncProgress::kDone, 0, 0);
	fRunning = false;
	fLibrary.NotifyChanged(kChangedSync);
	return result;
}


bool
SyncEngine::_Pull(ImmichClient& client, SyncResult& result,
	const std::atomic<bool>* cancelled, const ProgressFn& progress)
{
	// The first pass of a library asks for everything, whatever this session
	// acknowledged before; so does a pass after the server said "reset".
	bool full = fLibrary.Setting("immich.initial") != "done";
	for (int attempt = 0; attempt < 2; attempt++) {
		std::vector<SyncEvent> batch;
		std::map<std::string, std::string> acks;	// entity type -> latest
		std::set<std::string> seen;
		bool resetAsked = false;
		bool complete = false;
		int received = 0;
		int sinceAck = 0;

		auto apply = [&]() {
			if (batch.empty())
				return;
			bool listChanged = false;
			{
				std::lock_guard<std::recursive_mutex> lock(fLibrary.Lock());
				Transaction transaction(fLibrary.Db());
				for (auto& event : batch) {
					switch (event.kind) {
						case SyncEvent::kAsset:
						{
							if (full)
								seen.insert(event.asset.id);
							if (event.hidden || !event.asset.isImage) {
								// Not (or no longer) a picture of the
								// timeline.
								Photo photo;
								if (fLibrary.GetByImmichId(event.asset.id, photo)
									&& photo.path.empty()) {
									fLibrary.RemoveRemoteAsset(event.asset.id);
									result.removed++;
									listChanged = true;
								}
								break;
							}
							bool added = false;
							if (fLibrary.ApplyRemoteAsset(event.asset, &added) != 0) {
								if (added) {
									result.added++;
									listChanged = true;
								} else if (event.asset.trashed)
									listChanged = true;
							}
							break;
						}
						case SyncEvent::kAssetDelete:
						{
							Photo photo;
							if (fLibrary.GetByImmichId(event.assetId, photo)) {
								fLibrary.RemoveRemoteAsset(event.assetId);
								result.removed++;
								listChanged = true;
							}
							break;
						}
						case SyncEvent::kExif:
							fLibrary.ApplyRemoteExif(event.exif);
							break;
						default:
							break;
					}
				}
				transaction.Commit();
			}
			batch.clear();
			fLibrary.NotifyChanged(listChanged ? kChangedList : kChangedSync);
		};

		auto acknowledge = [&]() {
			std::vector<std::string> list;
			for (auto& ack : acks)
				list.push_back(ack.second);
			acks.clear();
			sinceAck = 0;
			std::string ignored;
			return list.empty() || client.Acknowledge(list, ignored);
		};

		bool ok = client.SyncStream(full, [&](const SyncEvent& event) {
			if (Cancelled(cancelled))
				return false;
			if (event.kind == SyncEvent::kReset) {
				resetAsked = true;
				return false;
			}
			if (event.kind == SyncEvent::kComplete)
				complete = true;
			else
				batch.push_back(event);
			if (!event.ack.empty() && !event.type.empty())
				acks[event.type] = event.ack;
			received++;
			sinceAck++;
			if (batch.size() >= 500) {
				apply();
				if (progress) {
					SyncProgress state;
					state.phase = SyncProgress::kReceiving;
					state.done = received;
					progress(state);
				}
			}
			// What is applied is acknowledged now and then, so that an
			// interrupted first sync goes on where it stopped.
			if (!full && sinceAck >= 5000 && batch.empty())
				acknowledge();
			return true;
		}, result.error, cancelled);
		apply();
		if (!ok)
			return false;
		if (resetAsked && !full) {
			full = true;
			continue;
		}
		if (Cancelled(cancelled))
			return true;

		if (full && complete) {
			// Everything the server has was listed: what the library still
			// links to something else is gone there.
			std::vector<std::string> gone;
			{
				std::lock_guard<std::recursive_mutex> lock(fLibrary.Lock());
				Statement statement = fLibrary.Db().Prepare(
					"SELECT immich_id FROM photos WHERE immich_id IS NOT NULL");
				while (statement.Step()) {
					std::string id = statement.Text(0);
					if (seen.find(id) == seen.end())
						gone.push_back(id);
				}
			}
			for (auto& id : gone) {
				fLibrary.RemoveRemoteAsset(id);
				result.removed++;
			}
			fLibrary.SetSetting("immich.initial", "done");
		}
		if (complete || !full)
			acknowledge();
		return true;
	}
	return true;
}


bool
SyncEngine::_PushChanges(ImmichClient& client, const SyncOptions& options,
	SyncResult& result)
{
	// Favourites.
	std::vector<Library::PendingFavorite> pending;
	fLibrary.ListPendingFavorites(pending);
	for (int value = 0; value < 2; value++) {
		std::vector<std::string> assets;
		std::vector<int64_t> photos;
		for (auto& item : pending) {
			if (item.favorite == (value == 1)) {
				assets.push_back(item.immichId);
				photos.push_back(item.id);
			}
		}
		for (size_t start = 0; start < assets.size(); start += 500) {
			size_t end = std::min(assets.size(), start + 500);
			std::vector<std::string> chunk(assets.begin() + start, assets.begin() + end);
			if (!client.SetFavorite(chunk, value == 1, result.error))
				return false;
			for (size_t i = start; i < end; i++)
				fLibrary.ClearPendingFavorite(photos[i]);
		}
	}

	// Photos deleted here for good.
	std::vector<std::string> removed;
	fLibrary.ListRemovedRemote(removed);
	if (!removed.empty()) {
		if (options.trashOnServer) {
			for (size_t start = 0; start < removed.size(); start += 500) {
				size_t end = std::min(removed.size(), start + 500);
				std::vector<std::string> chunk(removed.begin() + start,
					removed.begin() + end);
				if (!client.TrashAssets(chunk, result.error))
					return false;
				fLibrary.MarkRemovedRemote(chunk, 1);
			}
		} else
			fLibrary.MarkRemovedRemote(removed, 2);
	}
	return true;
}


bool
SyncEngine::_Upload(ImmichClient& client, SyncResult& result,
	const std::atomic<bool>* cancelled, const ProgressFn& progress)
{
	std::vector<int64_t> ids;
	fLibrary.ListForUpload(ids);
	int total = (int)ids.size();
	int done = 0;
	auto report = [&]() {
		if (progress) {
			SyncProgress state;
			state.phase = SyncProgress::kUploading;
			state.done = done;
			state.total = total;
			progress(state);
		}
	};
	for (size_t start = 0; start < ids.size() && !Cancelled(cancelled); start += 200) {
		size_t end = std::min(ids.size(), start + 200);
		std::vector<Photo> photos;
		std::vector<UploadCheck> checks;
		for (size_t i = start; i < end; i++) {
			Photo photo;
			if (!fLibrary.Get(ids[i], photo) || photo.path.empty()
				|| !photo.immichId.empty() || photo.trashed != 0)
				continue;
			UploadCheck check;
			check.key = std::to_string(photo.id);
			check.checksum = photo.sha1;
			checks.push_back(check);
			photos.push_back(photo);
		}
		if (!client.CheckUploads(checks, result.error))
			return false;
		for (size_t i = 0; i < photos.size() && !Cancelled(cancelled); i++) {
			const Photo& photo = photos[i];
			const UploadCheck& check = checks[i];
			report();
			done++;
			if (!check.accept) {
				if (!check.existingId.empty()) {
					// The server has this very file. Another photo of the
					// library may be linked to it already (the same file
					// imported twice): then this one stays local.
					Photo other;
					if (fLibrary.GetByImmichId(check.existingId, other))
						fLibrary.SetImmichLink(photo.id, "", kImmichExcluded);
					else {
						fLibrary.SetImmichLink(photo.id, check.existingId, kImmichSynced);
						result.linked++;
					}
				} else
					fLibrary.SetImmichLink(photo.id, "", kImmichExcluded);
				continue;
			}
			std::string path = fLibrary.OriginalPath(photo);
			int64_t created = photo.taken - LocalOffsetSeconds(photo.taken);
			int64_t modified = FileModified(path);
			if (modified == 0)
				modified = created;
			std::string assetId, error;
			bool duplicate = false;
			if (!client.Upload(path, photo.name, photo.sha1, created, modified,
					photo.favorite, assetId, duplicate, error, cancelled)) {
				if (Cancelled(cancelled))
					break;
				if (client.Unauthorized()) {
					result.error = error;
					return false;
				}
				result.failed++;
				if (result.firstFailure.empty())
					result.firstFailure = photo.name + ": " + error;
				continue;
			}
			Photo other;
			if (fLibrary.GetByImmichId(assetId, other) && other.id != photo.id)
				fLibrary.SetImmichLink(photo.id, "", kImmichExcluded);
			else {
				fLibrary.SetImmichLink(photo.id, assetId, kImmichSynced);
				if (duplicate)
					result.linked++;
				else
					result.uploaded++;
			}
		}
	}
	report();
	return true;
}


void
SyncEngine::_DownloadOriginals(SyncResult& result,
	const std::atomic<bool>* cancelled, const ProgressFn& progress)
{
	std::vector<PhotoBrief> photos;
	fLibrary.ListRemoteOnly(photos);
	int done = 0;
	for (auto& photo : photos) {
		if (Cancelled(cancelled))
			break;
		if (progress) {
			SyncProgress state;
			state.phase = SyncProgress::kDownloading;
			state.done = done;
			state.total = (int)photos.size();
			progress(state);
		}
		done++;
		std::string error;
		if (FetchOriginal(photo.id, error, cancelled))
			result.downloaded++;
		else if (!Cancelled(cancelled)) {
			result.failed++;
			if (result.firstFailure.empty())
				result.firstFailure = error;
		}
	}
}


bool
SyncEngine::FetchThumbnail(const std::string& immichId, const std::string& path,
	std::string& error, const std::atomic<bool>* cancelled)
{
	ImmichClient client(fHttp);
	_Configure(client, Options());
	if (!client.HasCredentials() || client.Server().empty()) {
		error = "no Immich server is set up";
		return false;
	}
	MakeDirs(DirName(path));
	return client.DownloadThumbnail(immichId, "thumbnail", path, error, cancelled);
}


bool
SyncEngine::FetchPreview(const std::string& immichId, const std::string& path,
	std::string& error, const std::atomic<bool>* cancelled)
{
	ImmichClient client(fHttp);
	_Configure(client, Options());
	if (!client.HasCredentials() || client.Server().empty()) {
		error = "no Immich server is set up";
		return false;
	}
	MakeDirs(DirName(path));
	return client.DownloadThumbnail(immichId, "preview", path, error, cancelled);
}


bool
SyncEngine::FetchOriginal(int64_t photoId, std::string& error,
	const std::atomic<bool>* cancelled)
{
	Photo photo;
	if (!fLibrary.Get(photoId, photo)) {
		error = "no such photo";
		return false;
	}
	if (!photo.path.empty())
		return true;
	if (photo.immichId.empty()) {
		error = "the original is nowhere";
		return false;
	}
	ImmichClient client(fHttp);
	_Configure(client, Options());
	if (!client.HasCredentials() || client.Server().empty()) {
		error = "no Immich server is set up";
		return false;
	}
	std::string directory = JoinPath(fLibrary.Directory(), "originals/.incoming");
	MakeDirs(directory);
	std::string temporary = JoinPath(directory, photo.immichId + "-" + photo.name);
	if (!client.DownloadOriginal(photo.immichId, temporary, error, cancelled))
		return false;
	ImportOptions options;
	options.move = true;
	options.name = photo.name;
	options.immichId = photo.immichId;
	options.takenFallback = photo.taken;
	ImportResult imported = fLibrary.ImportFile(temporary, options);
	RemoveFile(temporary);
	if (imported.id == 0) {
		error = imported.error;
		return false;
	}
	return true;
}

}	// namespace photos
