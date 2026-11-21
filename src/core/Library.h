// The photo library: a directory with the originals, rendered versions,
// thumbnails and a SQLite database.
//
//   <library>/library.db
//   <library>/originals/YYYY/MM/<name>     as imported, never modified
//   <library>/versions/<photo>/<n>.<ext>   edits rendered by Photos, saves from PhotoCraft
//   <library>/thumbs/<xx>/<photo>-<rev>.jpg
//   <library>/previews/<xx>/<photo>-<rev>.jpg   screen-size renderings (RAW, Immich)
//   <library>/edits/<photo>/               working files handed to PhotoCraft
//
// All methods may be called from any thread.
#pragma once

#include "Db.h"
#include "Exif.h"
#include "Util.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace photos {

// Where a photo stands with the Immich server.
enum ImmichState {
	kImmichNone = 0,		// not on the server (yet)
	kImmichSynced = 1,		// here and there
	kImmichRemoteOnly = 2,	// on the server; the original is not on this computer
	kImmichExcluded = 3		// never uploaded (the server refused it, or the user said so)
};

struct Photo {
	int64_t id = 0;
	std::string name;			// the original's file name
	std::string path;			// relative to the library; "" while the original is only on Immich
	MediaKind kind = kKindUnknown;
	int64_t size = 0;
	std::string sha1;			// base64, as Immich has it
	int width = 0;				// as shown (orientation applied) of the original
	int height = 0;
	int orientation = 1;
	int64_t taken = 0;			// local wall time
	int64_t imported = 0;		// UTC
	int64_t importId = 0;
	std::string make, model, lens;
	double iso = 0, exposureTime = 0, fNumber = 0, focalLength = 0;
	bool hasGps = false;
	double latitude = 0, longitude = 0;
	bool favorite = false;
	int64_t trashed = 0;		// UTC of the move to the trash; 0: not there
	int64_t version = 0;		// the version shown; 0: the original
	int64_t revision = 0;		// grows when the picture's look changes
	std::string immichId;
	int immichState = kImmichNone;
	std::string thumbhash;		// Immich's placeholder (base64)
	int facesVersion = 0;		// face model version the photo was scanned with
};

// What lists carry: enough to lay out and draw a grid.
struct PhotoBrief {
	int64_t id = 0;
	int64_t taken = 0;
	int64_t imported = 0;
	int64_t revision = 0;
	int width = 0;
	int height = 0;
	uint8_t kind = 0;
	bool favorite = false;
	bool remoteOnly = false;
	bool edited = false;
	bool trashed = false;
};

enum VersionKind {
	kVersionEdit = 1,		// rendered by Photos from `base` and `params`
	kVersionExternal = 2	// a file another application saved (PhotoCraft)
};

struct Version {
	int64_t id = 0;
	int64_t photoId = 0;
	int kind = kVersionEdit;
	int64_t created = 0;		// UTC
	int64_t base = 0;			// the version the edit starts from; 0: the original
	std::string path;			// the rendered picture, relative to the library
	std::string params;			// kVersionEdit: the adjustments (JSON, Edit.h)
	std::string source;			// kVersionExternal: "PhotoCraft"
	std::string document;		// kVersionExternal: the layered document kept next to it, if any
	int width = 0;
	int height = 0;
};

struct Import {
	int64_t id = 0;
	int64_t started = 0;
	std::string source;			// "Files", a camera's name, "Immich"
	int count = 0;
};

struct Person {
	int64_t id = 0;
	std::string name;
	int64_t coverFace = 0;
	int faceCount = 0;
	int photoCount = 0;
	bool hidden = false;
};

enum FaceSource {
	kFaceUnassigned = 0,
	kFaceAuto = 1,			// recognised: assigned by similarity to confirmed faces
	kFaceUser = 2			// the user said so
};

struct Face {
	int64_t id = 0;
	int64_t photoId = 0;
	float x = 0, y = 0, width = 0, height = 0;	// fractions of the picture
	float score = 0;
	float quality = 0;
	int64_t personId = 0;
	int source = kFaceUnassigned;
	int64_t cluster = 0;		// group of unnamed faces that look alike
	std::vector<float> embedding;
};

struct ImportOptions {
	int64_t importId = 0;
	bool move = false;			// take the file (a download) instead of copying it
	std::string name;			// the file's name when the source path has another
	int64_t takenFallback = 0;	// local wall time to use when the file says nothing
	std::string itemKey;		// remembered as imported (cameras: "only new")
	std::string immichId;		// the file is this asset's original
};

struct ImportResult {
	int64_t id = 0;
	bool added = false;
	bool duplicate = false;		// the same picture was there already
	bool restored = false;		// ... in the trash: brought back
	std::string error;
};

// An asset as the Immich server describes it.
struct RemoteAsset {
	std::string id;
	std::string name;
	std::string checksum;		// base64 SHA-1
	std::string thumbhash;
	int64_t taken = 0;			// local wall time
	int width = 0, height = 0;
	bool favorite = false;
	bool trashed = false;
	bool isImage = true;
	int64_t uploaded = 0;		// when the server got it (UTC)
};

struct RemoteExif {
	std::string assetId;
	int64_t size = 0;
	std::string make, model, lens;
	double iso = 0, exposureTime = 0, fNumber = 0, focalLength = 0;
	bool hasGps = false;
	double latitude = 0, longitude = 0;
	int width = 0, height = 0;	// 0: unknown
	int orientation = 0;		// 0: unknown
	int64_t taken = 0;			// local wall time, 0: unknown
};

enum PhotoFilter {
	kFilterAll,			// the timeline: everything not in the trash
	kFilterRecent,		// the last import sessions, newest first
	kFilterFavorites,
	kFilterTrash,
	kFilterPerson,		// argument: person id
	kFilterImport,		// argument: import id
	kFilterEdited,
	kFilterLocalOnly,	// not on Immich
	kFilterNeedsFaces	// argument: model version
};

enum LibraryChange {
	kChangedList = 1,		// photos added, removed, moved to or from the trash
	kChangedPhoto = 2,		// one photo's look or details ("id")
	kChangedPeople = 4,
	kChangedSync = 8
};

// The square part of a picture to show for a face: its box with some room
// around it, kept inside the picture. Fractions of the picture.
void FaceCropRect(const Face& face, int imageWidth, int imageHeight,
	float& x, float& y, float& w, float& h);

class Library {
public:
	Library();
	~Library();

	bool Open(const std::string& directory, std::string* error = nullptr);
	void Close();
	bool IsOpen() const { return fDb.IsOpen(); }
	const std::string& Directory() const { return fDirectory; }
	std::string AbsolutePath(const std::string& relative) const;

	// Called after changes, on the thread that made them, outside the lock.
	typedef std::function<void(uint32_t changes, int64_t photoId)> Listener;
	void SetListener(Listener listener);
	// For callers that batch changes themselves (sync).
	void NotifyChanged(uint32_t changes, int64_t photoId = 0);

	// Fills what the file itself says: called for kinds the library cannot
	// read (RAW, HEIF). Returns false when it cannot either.
	typedef std::function<bool(const std::string& path, MediaKind kind,
		Metadata& metadata)> MetadataReader;
	void SetMetadataReader(MetadataReader reader);

	// --- Photos ---
	void List(PhotoFilter filter, int64_t argument, std::vector<PhotoBrief>& photos);
	bool Get(int64_t id, Photo& photo);
	bool GetBySha1(const std::string& sha1, Photo& photo);
	bool GetByImmichId(const std::string& immichId, Photo& photo);
	int Count(PhotoFilter filter, int64_t argument = 0);

	int64_t BeginImport(const std::string& source);
	ImportResult ImportFile(const std::string& path, const ImportOptions& options);
	void ListImports(std::vector<Import>& imports);
	// "Only new": was an item with this key imported before (even if the
	// photo was deleted since), or is a photo of this name and size there?
	bool WasImported(const std::string& itemKey, const std::string& name, int64_t size);

	void SetFavorite(int64_t id, bool favorite);
	void SetTaken(int64_t id, int64_t taken);
	void MoveToTrash(const std::vector<int64_t>& ids);
	void RestoreFromTrash(const std::vector<int64_t>& ids);
	// Removes photos for good: files, versions, faces. With keepRemoteMark the
	// library remembers assets that are on Immich so that sync does not bring
	// them back.
	void DeletePermanently(const std::vector<int64_t>& ids);
	void EmptyTrash();
	// Deletes what has been in the trash for longer than `days`.
	void ExpireTrash(int days);

	// The file a photo is shown from: its current version, else the original;
	// "" when neither is on this computer.
	std::string DisplayPath(const Photo& photo);
	std::string OriginalPath(const Photo& photo) const;
	std::string ThumbPath(int64_t id, int64_t revision) const;
	std::string PreviewPath(int64_t id, int64_t revision) const;
	std::string EditsDirectory(int64_t id) const;
	// A small picture of a face, saved when its photo was scanned.
	std::string FaceThumbPath(int64_t faceId) const;
	// Drops cached thumbnails and previews of older revisions.
	void PruneCaches(int64_t id, int64_t keepRevision);

	// --- Versions ---
	void ListVersions(int64_t photoId, std::vector<Version>& versions);
	bool GetVersion(int64_t versionId, Version& version);
	// Adds a version from `file` (copied or moved into the library) and makes
	// it the one shown. Returns its id, 0 on failure.
	int64_t AddVersion(int64_t photoId, const Version& version,
		const std::string& file, bool move);
	// Replaces the picture and adjustments of an edit version in place.
	bool UpdateEditVersion(int64_t versionId, const std::string& params,
		const std::string& file, int width, int height);
	bool SetCurrentVersion(int64_t photoId, int64_t versionId);	// 0: original
	bool DeleteVersion(int64_t versionId);

	// --- Faces and people ---
	// Replaces the faces found in a photo (keeping people of faces that are
	// in the same place) and marks it scanned with `modelVersion`.
	void SetFaces(int64_t photoId, std::vector<Face>& faces, int modelVersion);
	void ListFaces(int64_t photoId, std::vector<Face>& faces, bool withEmbeddings = false);
	void ListAllFaces(std::vector<Face>& faces, bool withEmbeddings = true);
	bool GetFace(int64_t faceId, Face& face, bool withEmbedding = false);
	void ListPeople(std::vector<Person>& people, bool withHidden = false);
	bool GetPerson(int64_t id, Person& person);
	int64_t FindPerson(const std::string& name);
	int64_t CreatePerson(const std::string& name);
	void RenamePerson(int64_t id, const std::string& name);
	void SetPersonHidden(int64_t id, bool hidden);
	void SetPersonCover(int64_t id, int64_t faceId);
	void MergePeople(int64_t from, int64_t into);
	void DeletePerson(int64_t id);			// its faces become unassigned
	void AssignFace(int64_t faceId, int64_t personId, int source);
	void AssignFaces(const std::vector<int64_t>& faceIds, int64_t personId, int source);
	void SetFaceCluster(int64_t faceId, int64_t cluster);
	void SetFaceClusters(const std::vector<std::pair<int64_t, int64_t>>& faceClusters);
	// "This is not her": the pair is never suggested or assigned again.
	void RejectFace(int64_t faceId, int64_t personId);
	bool IsRejected(int64_t faceId, int64_t personId);
	void ListRejections(std::vector<std::pair<int64_t, int64_t>>& pairs);
	void DeleteFace(int64_t faceId);

	// --- Immich ---
	// Records what the server says about an asset. Returns the photo's id (0
	// when the asset is skipped: a video, or deleted here on purpose).
	int64_t ApplyRemoteAsset(const RemoteAsset& asset, bool* added = nullptr);
	void ApplyRemoteExif(const RemoteExif& exif);
	void RemoveRemoteAsset(const std::string& immichId);
	void SetImmichLink(int64_t id, const std::string& immichId, int state);
	// The original of a remote-only photo arrived (ImportFile with immichId
	// does this); or it was dropped again to save space.
	bool DropOriginal(int64_t id);
	void ListForUpload(std::vector<int64_t>& ids);
	// Favourites changed here that the server has not heard of yet, as
	// (photo id, asset id, favourite); and their acknowledgement.
	struct PendingFavorite {
		int64_t id;
		std::string immichId;
		bool favorite;
	};
	void ListPendingFavorites(std::vector<PendingFavorite>& pending);
	void ClearPendingFavorite(int64_t id);
	// Assets of photos deleted here for good, not yet dealt with on the
	// server; MarkRemovedRemote(ids, 1) after trashing them there, 2 when the
	// user does not want that.
	void ListRemovedRemote(std::vector<std::string>& immichIds);
	void MarkRemovedRemote(const std::vector<std::string>& immichIds, int state);
	// Photos whose original is only on the server, newest first.
	void ListRemoteOnly(std::vector<PhotoBrief>& photos);
	// Forgets every link to the server (another account or server is used).
	void ForgetImmich();

	// --- Settings kept with the library ---
	std::string Setting(const std::string& key, const std::string& fallback = "");
	void SetSetting(const std::string& key, const std::string& value);

	Database& Db() { return fDb; }
	std::recursive_mutex& Lock() { return fLock; }

private:
	bool _CreateSchema();
	bool _ReadPhoto(Statement& statement, Photo& photo);
	void _Notify(uint32_t changes, int64_t photoId = 0);
	void _BumpRevision(int64_t id);
	void _DeleteFiles(const Photo& photo);
	std::string _StoreOriginal(const std::string& source, const std::string& name,
		int64_t taken, bool move);
	void _FillMetadata(const std::string& path, MediaKind kind, Metadata& metadata);

	Database fDb;
	std::string fDirectory;
	std::recursive_mutex fLock;
	Listener fListener;
	MetadataReader fMetadataReader;
};

}	// namespace photos
