#include "Library.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace photos {

namespace {

const int kSchemaVersion = 1;

const char* const kPhotoColumns =
	"id, name, path, kind, size, sha1, width, height, orientation, taken, "
	"imported, import_id, make, model, lens, iso, exposure, fnumber, focal, "
	"has_gps, latitude, longitude, favorite, trashed, version, revision, "
	"immich_id, immich_state, thumbhash, faces_version";

const char* const kBriefColumns =
	"id, taken, imported, revision, width, height, kind, favorite, "
	"immich_state, version, trashed, path";

const char* const kVersionColumns =
	"id, photo_id, kind, created, base, path, params, source, document, width, height";

const char* const kFaceColumns =
	"id, photo_id, x, y, w, h, score, quality, person_id, source, cluster";


void
ReadBrief(Statement& statement, PhotoBrief& brief)
{
	brief.id = statement.Int(0);
	brief.taken = statement.Int(1);
	brief.imported = statement.Int(2);
	brief.revision = statement.Int(3);
	brief.width = (int)statement.Int(4);
	brief.height = (int)statement.Int(5);
	brief.kind = (uint8_t)statement.Int(6);
	brief.favorite = statement.Int(7) != 0;
	brief.remoteOnly = statement.Text(11).empty();
	brief.edited = statement.Int(9) != 0;
	brief.trashed = statement.Int(10) != 0;
}


void
ReadVersion(Statement& statement, Version& version)
{
	version.id = statement.Int(0);
	version.photoId = statement.Int(1);
	version.kind = (int)statement.Int(2);
	version.created = statement.Int(3);
	version.base = statement.Int(4);
	version.path = statement.Text(5);
	version.params = statement.Text(6);
	version.source = statement.Text(7);
	version.document = statement.Text(8);
	version.width = (int)statement.Int(9);
	version.height = (int)statement.Int(10);
}


void
ReadFace(Statement& statement, Face& face, int embeddingColumn)
{
	face.id = statement.Int(0);
	face.photoId = statement.Int(1);
	face.x = (float)statement.Double(2);
	face.y = (float)statement.Double(3);
	face.width = (float)statement.Double(4);
	face.height = (float)statement.Double(5);
	face.score = (float)statement.Double(6);
	face.quality = (float)statement.Double(7);
	face.personId = statement.Int(8);
	face.source = (int)statement.Int(9);
	face.cluster = statement.Int(10);
	face.embedding.clear();
	if (embeddingColumn >= 0) {
		size_t size;
		const void* blob = statement.Blob(embeddingColumn, size);
		if (blob != nullptr && size >= sizeof(float)) {
			face.embedding.resize(size / sizeof(float));
			memcpy(face.embedding.data(), blob, face.embedding.size() * sizeof(float));
		}
	}
}


float
Overlap(const Face& a, const Face& b)
{
	float left = std::max(a.x, b.x);
	float top = std::max(a.y, b.y);
	float right = std::min(a.x + a.width, b.x + b.width);
	float bottom = std::min(a.y + a.height, b.y + b.height);
	if (right <= left || bottom <= top)
		return 0;
	float both = (right - left) * (bottom - top);
	float either = a.width * a.height + b.width * b.height - both;
	return either > 0 ? both / either : 0;
}


std::string
Shard(int64_t id)
{
	char text[8];
	snprintf(text, sizeof(text), "%02x", (unsigned)(id & 0xff));
	return text;
}

}	// namespace


Library::Library()
{
}


Library::~Library()
{
	Close();
}


bool
Library::Open(const std::string& directory, std::string* error)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Close();
	fDirectory = directory;
	static const char* const kDirectories[] = {"", "originals", "versions",
		"thumbs", "previews", "edits", "faces", nullptr};
	for (int i = 0; kDirectories[i] != nullptr; i++) {
		if (!MakeDirs(JoinPath(directory, kDirectories[i]))) {
			if (error != nullptr)
				*error = "cannot create " + JoinPath(directory, kDirectories[i]);
			return false;
		}
	}
	if (!fDb.Open(JoinPath(directory, "library.db"), error))
		return false;
	if (!_CreateSchema()) {
		if (error != nullptr)
			*error = "cannot set up the database: " + fDb.LastError();
		fDb.Close();
		return false;
	}
	return true;
}


void
Library::Close()
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	fDb.Close();
}


bool
Library::_CreateSchema()
{
	Statement version = fDb.Prepare("PRAGMA user_version");
	int current = version.Step() ? (int)version.Int(0) : 0;
	version = Statement();
	if (current > kSchemaVersion) {
		fprintf(stderr, "photos: the library was made by a newer version\n");
		return false;
	}
	if (current == kSchemaVersion)
		return true;

	Transaction transaction(fDb);
	static const char* const kSchema[] = {
		"CREATE TABLE IF NOT EXISTS photos("
			"id INTEGER PRIMARY KEY AUTOINCREMENT,"
			"name TEXT NOT NULL,"
			"path TEXT NOT NULL DEFAULT '',"
			"kind INTEGER NOT NULL DEFAULT 0,"
			"size INTEGER NOT NULL DEFAULT 0,"
			"sha1 TEXT NOT NULL DEFAULT '',"
			"width INTEGER NOT NULL DEFAULT 0,"
			"height INTEGER NOT NULL DEFAULT 0,"
			"orientation INTEGER NOT NULL DEFAULT 1,"
			"taken INTEGER NOT NULL DEFAULT 0,"
			"imported INTEGER NOT NULL DEFAULT 0,"
			"import_id INTEGER NOT NULL DEFAULT 0,"
			"make TEXT NOT NULL DEFAULT '',"
			"model TEXT NOT NULL DEFAULT '',"
			"lens TEXT NOT NULL DEFAULT '',"
			"iso REAL NOT NULL DEFAULT 0,"
			"exposure REAL NOT NULL DEFAULT 0,"
			"fnumber REAL NOT NULL DEFAULT 0,"
			"focal REAL NOT NULL DEFAULT 0,"
			"has_gps INTEGER NOT NULL DEFAULT 0,"
			"latitude REAL NOT NULL DEFAULT 0,"
			"longitude REAL NOT NULL DEFAULT 0,"
			"favorite INTEGER NOT NULL DEFAULT 0,"
			"trashed INTEGER NOT NULL DEFAULT 0,"
			"version INTEGER NOT NULL DEFAULT 0,"
			"revision INTEGER NOT NULL DEFAULT 1,"
			"immich_id TEXT,"
			"immich_state INTEGER NOT NULL DEFAULT 0,"
			"thumbhash TEXT NOT NULL DEFAULT '',"
			"faces_version INTEGER NOT NULL DEFAULT 0,"
			"dirty INTEGER NOT NULL DEFAULT 0)",
		"CREATE INDEX IF NOT EXISTS photos_taken ON photos(taken)",
		"CREATE INDEX IF NOT EXISTS photos_imported ON photos(imported)",
		"CREATE INDEX IF NOT EXISTS photos_sha1 ON photos(sha1)",
		"CREATE UNIQUE INDEX IF NOT EXISTS photos_immich ON photos(immich_id)"
			" WHERE immich_id IS NOT NULL",
		"CREATE INDEX IF NOT EXISTS photos_name ON photos(name COLLATE NOCASE, size)",
		"CREATE TABLE IF NOT EXISTS versions("
			"id INTEGER PRIMARY KEY AUTOINCREMENT,"
			"photo_id INTEGER NOT NULL,"
			"kind INTEGER NOT NULL,"
			"created INTEGER NOT NULL,"
			"base INTEGER NOT NULL DEFAULT 0,"
			"path TEXT NOT NULL DEFAULT '',"
			"params TEXT NOT NULL DEFAULT '',"
			"source TEXT NOT NULL DEFAULT '',"
			"document TEXT NOT NULL DEFAULT '',"
			"width INTEGER NOT NULL DEFAULT 0,"
			"height INTEGER NOT NULL DEFAULT 0)",
		"CREATE INDEX IF NOT EXISTS versions_photo ON versions(photo_id)",
		"CREATE TABLE IF NOT EXISTS imports("
			"id INTEGER PRIMARY KEY AUTOINCREMENT,"
			"started INTEGER NOT NULL,"
			"source TEXT NOT NULL DEFAULT '')",
		"CREATE TABLE IF NOT EXISTS imported_items(key TEXT PRIMARY KEY)",
		"CREATE TABLE IF NOT EXISTS people("
			"id INTEGER PRIMARY KEY AUTOINCREMENT,"
			"name TEXT NOT NULL DEFAULT '',"
			"cover_face INTEGER NOT NULL DEFAULT 0,"
			"hidden INTEGER NOT NULL DEFAULT 0,"
			"created INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE IF NOT EXISTS faces("
			"id INTEGER PRIMARY KEY AUTOINCREMENT,"
			"photo_id INTEGER NOT NULL,"
			"x REAL NOT NULL, y REAL NOT NULL, w REAL NOT NULL, h REAL NOT NULL,"
			"score REAL NOT NULL DEFAULT 0,"
			"quality REAL NOT NULL DEFAULT 0,"
			"person_id INTEGER NOT NULL DEFAULT 0,"
			"source INTEGER NOT NULL DEFAULT 0,"
			"cluster INTEGER NOT NULL DEFAULT 0,"
			"embedding BLOB)",
		"CREATE INDEX IF NOT EXISTS faces_photo ON faces(photo_id)",
		"CREATE INDEX IF NOT EXISTS faces_person ON faces(person_id)",
		"CREATE TABLE IF NOT EXISTS face_rejections("
			"face_id INTEGER NOT NULL, person_id INTEGER NOT NULL,"
			"PRIMARY KEY(face_id, person_id))",
		// Assets deleted here on purpose: sync does not bring them back, and
		// may move them to the server's trash (pushed = 1 once done).
		"CREATE TABLE IF NOT EXISTS removed_remote("
			"immich_id TEXT PRIMARY KEY, pushed INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT)",
		nullptr
	};
	for (int i = 0; kSchema[i] != nullptr; i++) {
		if (!fDb.Exec(kSchema[i]))
			return false;
	}
	if (!fDb.Exec("PRAGMA user_version = " + std::to_string(kSchemaVersion)))
		return false;
	return transaction.Commit();
}


std::string
Library::AbsolutePath(const std::string& relative) const
{
	if (relative.empty())
		return std::string();
	return JoinPath(fDirectory, relative);
}


void
Library::SetListener(Listener listener)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	fListener = listener;
}


void
Library::SetMetadataReader(MetadataReader reader)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	fMetadataReader = reader;
}


void
Library::_Notify(uint32_t changes, int64_t photoId)
{
	Listener listener;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		listener = fListener;
	}
	if (listener)
		listener(changes, photoId);
}


void
Library::NotifyChanged(uint32_t changes, int64_t photoId)
{
	_Notify(changes, photoId);
}


// --- Photos -----------------------------------------------------------------

bool
Library::_ReadPhoto(Statement& statement, Photo& photo)
{
	photo.id = statement.Int(0);
	photo.name = statement.Text(1);
	photo.path = statement.Text(2);
	photo.kind = (MediaKind)statement.Int(3);
	photo.size = statement.Int(4);
	photo.sha1 = statement.Text(5);
	photo.width = (int)statement.Int(6);
	photo.height = (int)statement.Int(7);
	photo.orientation = (int)statement.Int(8);
	photo.taken = statement.Int(9);
	photo.imported = statement.Int(10);
	photo.importId = statement.Int(11);
	photo.make = statement.Text(12);
	photo.model = statement.Text(13);
	photo.lens = statement.Text(14);
	photo.iso = statement.Double(15);
	photo.exposureTime = statement.Double(16);
	photo.fNumber = statement.Double(17);
	photo.focalLength = statement.Double(18);
	photo.hasGps = statement.Int(19) != 0;
	photo.latitude = statement.Double(20);
	photo.longitude = statement.Double(21);
	photo.favorite = statement.Int(22) != 0;
	photo.trashed = statement.Int(23);
	photo.version = statement.Int(24);
	photo.revision = statement.Int(25);
	photo.immichId = statement.Text(26);
	photo.immichState = (int)statement.Int(27);
	photo.thumbhash = statement.Text(28);
	photo.facesVersion = (int)statement.Int(29);
	return true;
}


void
Library::List(PhotoFilter filter, int64_t argument, std::vector<PhotoBrief>& photos)
{
	photos.clear();
	std::string sql = std::string("SELECT ") + kBriefColumns + " FROM photos WHERE ";
	bool bind = false;
	switch (filter) {
		case kFilterAll:
			sql += "trashed = 0 ORDER BY taken DESC, id DESC";
			break;
		case kFilterRecent:
			sql += "trashed = 0 ORDER BY imported DESC, taken DESC, id DESC LIMIT 600";
			break;
		case kFilterFavorites:
			sql += "trashed = 0 AND favorite != 0 ORDER BY taken DESC, id DESC";
			break;
		case kFilterTrash:
			sql += "trashed != 0 ORDER BY trashed DESC, id DESC";
			break;
		case kFilterPerson:
			sql += "trashed = 0 AND id IN (SELECT photo_id FROM faces WHERE person_id = ?1)"
				" ORDER BY taken DESC, id DESC";
			bind = true;
			break;
		case kFilterImport:
			sql += "trashed = 0 AND import_id = ?1 ORDER BY taken DESC, id DESC";
			bind = true;
			break;
		case kFilterEdited:
			sql += "trashed = 0 AND version != 0 ORDER BY taken DESC, id DESC";
			break;
		case kFilterLocalOnly:
			sql += "trashed = 0 AND immich_id IS NULL ORDER BY taken DESC, id DESC";
			break;
		case kFilterNeedsFaces:
			sql += "trashed = 0 AND faces_version != ?1 AND kind != "
				+ std::to_string((int)kKindVideo) + " ORDER BY taken DESC, id DESC";
			bind = true;
			break;
	}
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(sql);
	if (bind)
		statement.Bind(1, argument);
	while (statement.Step()) {
		photos.emplace_back();
		ReadBrief(statement, photos.back());
	}
}


int
Library::Count(PhotoFilter filter, int64_t argument)
{
	std::string sql = "SELECT COUNT(*) FROM photos WHERE ";
	bool bind = false;
	switch (filter) {
		case kFilterAll: sql += "trashed = 0"; break;
		case kFilterRecent: sql += "trashed = 0"; break;
		case kFilterFavorites: sql += "trashed = 0 AND favorite != 0"; break;
		case kFilterTrash: sql += "trashed != 0"; break;
		case kFilterPerson:
			sql += "trashed = 0 AND id IN (SELECT photo_id FROM faces WHERE person_id = ?1)";
			bind = true;
			break;
		case kFilterImport: sql += "trashed = 0 AND import_id = ?1"; bind = true; break;
		case kFilterEdited: sql += "trashed = 0 AND version != 0"; break;
		case kFilterLocalOnly: sql += "trashed = 0 AND immich_id IS NULL"; break;
		case kFilterNeedsFaces:
			sql += "trashed = 0 AND faces_version != ?1 AND kind != "
				+ std::to_string((int)kKindVideo);
			bind = true;
			break;
	}
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(sql);
	if (bind)
		statement.Bind(1, argument);
	return statement.Step() ? (int)statement.Int(0) : 0;
}


bool
Library::Get(int64_t id, Photo& photo)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(std::string("SELECT ") + kPhotoColumns
		+ " FROM photos WHERE id = ?1");
	statement.Bind(1, id);
	return statement.Step() && _ReadPhoto(statement, photo);
}


bool
Library::GetBySha1(const std::string& sha1, Photo& photo)
{
	if (sha1.empty())
		return false;
	std::lock_guard<std::recursive_mutex> lock(fLock);
	// A photo that is not in the trash first.
	Statement statement = fDb.Prepare(std::string("SELECT ") + kPhotoColumns
		+ " FROM photos WHERE sha1 = ?1 ORDER BY trashed != 0, id LIMIT 1");
	statement.Bind(1, sha1);
	return statement.Step() && _ReadPhoto(statement, photo);
}


bool
Library::GetByImmichId(const std::string& immichId, Photo& photo)
{
	if (immichId.empty())
		return false;
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(std::string("SELECT ") + kPhotoColumns
		+ " FROM photos WHERE immich_id = ?1");
	statement.Bind(1, immichId);
	return statement.Step() && _ReadPhoto(statement, photo);
}


int64_t
Library::BeginImport(const std::string& source)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"INSERT INTO imports(started, source) VALUES(?1, ?2)");
	statement.Bind(1, NowSeconds()).Bind(2, source);
	return statement.Run() ? fDb.LastInsertId() : 0;
}


void
Library::ListImports(std::vector<Import>& imports)
{
	imports.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"SELECT i.id, i.started, i.source, COUNT(p.id) FROM imports i"
		" JOIN photos p ON p.import_id = i.id AND p.trashed = 0"
		" GROUP BY i.id ORDER BY i.started DESC, i.id DESC");
	while (statement.Step()) {
		Import import;
		import.id = statement.Int(0);
		import.started = statement.Int(1);
		import.source = statement.Text(2);
		import.count = (int)statement.Int(3);
		imports.push_back(import);
	}
}


bool
Library::WasImported(const std::string& itemKey, const std::string& name, int64_t size)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	if (!itemKey.empty()) {
		Statement statement = fDb.Prepare("SELECT 1 FROM imported_items WHERE key = ?1");
		statement.Bind(1, itemKey);
		if (statement.Step())
			return true;
	}
	if (!name.empty() && size > 0) {
		Statement statement = fDb.Prepare(
			"SELECT 1 FROM photos WHERE name = ?1 COLLATE NOCASE AND size = ?2 LIMIT 1");
		statement.Bind(1, name).Bind(2, size);
		if (statement.Step())
			return true;
	}
	return false;
}


// False when the file cannot be read as its kind by whoever reads that kind
// (without a reader for RAW and HEIF files nobody can tell: true).
bool
Library::_FillMetadata(const std::string& path, MediaKind kind, Metadata& metadata)
{
	bool known = false;
	bool asked = false;
	if (kind == kKindJpeg)
		known = ReadJpegMetadata(path, metadata);
	if (!known) {
		MetadataReader reader;
		{
			std::lock_guard<std::recursive_mutex> lock(fLock);
			reader = fMetadataReader;
		}
		if (reader) {
			asked = true;
			known = reader(path, kind, metadata);
		}
	}
	if (!known || metadata.width == 0)
		ReadImageSize(path, metadata.width, metadata.height);
	return known || !asked;
}


std::string
Library::_StoreOriginal(const std::string& source, const std::string& name,
	int64_t taken, bool move)
{
	CivilTime civil = SecondsToCivil(taken);
	char folder[64];
	snprintf(folder, sizeof(folder), "originals/%04d/%02d", civil.year, civil.month);
	std::string directory = JoinPath(fDirectory, folder);
	if (!MakeDirs(directory))
		return std::string();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	std::string unique = UniqueName(directory, name);
	std::string target = JoinPath(directory, unique);
	bool ok = false;
	if (move) {
		ok = rename(source.c_str(), target.c_str()) == 0;
		if (!ok) {
			ok = CopyFile(source, target);
			if (ok)
				RemoveFile(source);
		}
	} else
		ok = CopyFile(source, target);
	if (!ok)
		return std::string();
	return JoinPath(folder, unique);
}


ImportResult
Library::ImportFile(const std::string& path, const ImportOptions& options)
{
	ImportResult result;
	std::string name = options.name.empty() ? BaseName(path) : options.name;
	MediaKind kind = KindOfName(name);
	if (kind == kKindUnknown || kind == kKindVideo) {
		result.error = kind == kKindVideo ? "videos are not imported"
			: "not a picture Photos knows";
		return result;
	}
	int64_t size = FileSize(path);
	if (size <= 0) {
		result.error = "cannot read the file";
		return result;
	}
	// What the server keeps is taken as it is; anything else has to be what
	// its name says.
	if (options.immichId.empty() && !LooksLikePicture(path, kind)) {
		result.error = "the file is damaged or not a picture";
		return result;
	}
	std::string sha1 = Sha1OfFile(path);
	if (sha1.empty()) {
		result.error = "cannot read the file";
		return result;
	}

	auto remember = [&]() {
		if (options.itemKey.empty())
			return;
		Statement statement = fDb.Prepare(
			"INSERT OR IGNORE INTO imported_items(key) VALUES(?1)");
		statement.Bind(1, options.itemKey);
		statement.Run();
	};

	// The same file already in the library?
	Photo existing;
	bool found = false;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		if (!options.immichId.empty())
			found = GetByImmichId(options.immichId, existing);
		if (!found)
			found = GetBySha1(sha1, existing);
		if (found && !existing.path.empty()) {
			remember();
			result.id = existing.id;
			result.duplicate = true;
			if (existing.trashed != 0) {
				Statement statement = fDb.Prepare(
					"UPDATE photos SET trashed = 0 WHERE id = ?1");
				statement.Bind(1, existing.id).Run();
				result.restored = true;
			}
			if (options.move)
				RemoveFile(path);
		}
	}
	if (result.duplicate) {
		if (result.restored)
			_Notify(kChangedList);
		return result;
	}

	Metadata metadata;
	bool readable = _FillMetadata(path, kind, metadata);
	if (!readable && options.immichId.empty() && !found
		&& (kind == kKindRaw || kind == kKindHeic)) {
		result.error = kind == kKindRaw ? "not a RAW file Photos can read"
			: "not a HEIF picture Photos can read";
		return result;
	}
	int64_t taken = metadata.taken;
	if (taken == 0 && found)
		taken = existing.taken;
	if (taken == 0)
		taken = options.takenFallback;
	if (taken == 0) {
		int64_t modified = FileModified(path);
		taken = modified + LocalOffsetSeconds(modified);
	}

	std::string stored = _StoreOriginal(path, name, taken, options.move);
	if (stored.empty()) {
		result.error = "cannot copy the file into the library";
		return result;
	}

	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		if (found) {
			// A photo known from Immich gets its original.
			Statement statement = fDb.Prepare(
				"UPDATE photos SET path = ?1, kind = ?2, size = ?3, sha1 = ?4,"
				" width = CASE WHEN ?5 > 0 THEN ?5 ELSE width END,"
				" height = CASE WHEN ?6 > 0 THEN ?6 ELSE height END,"
				" orientation = ?7, immich_state = ?8, revision = revision + 1"
				" WHERE id = ?9");
			statement.Bind(1, stored).Bind(2, (int)kind).Bind(3, size).Bind(4, sha1)
				.Bind(5, metadata.DisplayWidth()).Bind(6, metadata.DisplayHeight())
				.Bind(7, metadata.orientation)
				.Bind(8, existing.immichId.empty() ? (int)kImmichNone : (int)kImmichSynced)
				.Bind(9, existing.id);
			statement.Run();
			result.id = existing.id;
		} else {
			Statement statement = fDb.Prepare(
				"INSERT INTO photos(name, path, kind, size, sha1, width, height,"
				" orientation, taken, imported, import_id, make, model, lens, iso,"
				" exposure, fnumber, focal, has_gps, latitude, longitude)"
				" VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13,"
				" ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21)");
			statement.Bind(1, name).Bind(2, stored).Bind(3, (int)kind).Bind(4, size)
				.Bind(5, sha1).Bind(6, metadata.DisplayWidth())
				.Bind(7, metadata.DisplayHeight()).Bind(8, metadata.orientation)
				.Bind(9, taken).Bind(10, NowSeconds()).Bind(11, options.importId)
				.Bind(12, metadata.make).Bind(13, metadata.model).Bind(14, metadata.lens)
				.Bind(15, metadata.iso).Bind(16, metadata.exposureTime)
				.Bind(17, metadata.fNumber).Bind(18, metadata.focalLength)
				.Bind(19, metadata.hasGps ? 1 : 0).Bind(20, metadata.latitude)
				.Bind(21, metadata.longitude);
			if (!statement.Run()) {
				RemoveFile(JoinPath(fDirectory, stored));
				result.error = "database: " + fDb.LastError();
				return result;
			}
			result.id = fDb.LastInsertId();
			result.added = true;
		}
		remember();
		transaction.Commit();
	}
	_Notify(result.added ? kChangedList : kChangedPhoto, result.id);
	return result;
}


void
Library::SetFavorite(int64_t id, bool favorite)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		// dirty bit 1: tell the server.
		Statement statement = fDb.Prepare(
			"UPDATE photos SET favorite = ?1, dirty = dirty | 1"
			" WHERE id = ?2 AND favorite != ?1");
		statement.Bind(1, favorite ? 1 : 0).Bind(2, id).Run();
	}
	_Notify(kChangedPhoto, id);
}


void
Library::SetTaken(int64_t id, int64_t taken)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare("UPDATE photos SET taken = ?1 WHERE id = ?2");
		statement.Bind(1, taken).Bind(2, id).Run();
	}
	_Notify(kChangedList);
}


void
Library::MoveToTrash(const std::vector<int64_t>& ids)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		Statement statement = fDb.Prepare(
			"UPDATE photos SET trashed = ?1 WHERE id = ?2 AND trashed = 0");
		int64_t now = NowSeconds();
		for (int64_t id : ids) {
			statement.Reset();
			statement.Bind(1, now).Bind(2, id).Run();
		}
		transaction.Commit();
	}
	_Notify(kChangedList | kChangedPeople);
}


void
Library::RestoreFromTrash(const std::vector<int64_t>& ids)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		Statement statement = fDb.Prepare("UPDATE photos SET trashed = 0 WHERE id = ?1");
		for (int64_t id : ids) {
			statement.Reset();
			statement.Bind(1, id).Run();
		}
		transaction.Commit();
	}
	_Notify(kChangedList | kChangedPeople);
}


void
Library::_DeleteFiles(const Photo& photo)
{
	if (!photo.path.empty())
		RemoveFile(JoinPath(fDirectory, photo.path));
	RemoveTree(JoinPath(fDirectory, "versions/" + std::to_string(photo.id)));
	RemoveTree(EditsDirectory(photo.id));
	PruneCaches(photo.id, -1);
}


void
Library::DeletePermanently(const std::vector<int64_t>& ids)
{
	for (int64_t id : ids) {
		Photo photo;
		if (!Get(id, photo))
			continue;
		std::vector<Face> faces;
		ListFaces(id, faces);
		for (auto& face : faces)
			RemoveFile(FaceThumbPath(face.id));
		{
			std::lock_guard<std::recursive_mutex> lock(fLock);
			Transaction transaction(fDb);
			if (!photo.immichId.empty()) {
				Statement statement = fDb.Prepare(
					"INSERT OR IGNORE INTO removed_remote(immich_id) VALUES(?1)");
				statement.Bind(1, photo.immichId).Run();
			}
			Statement faces = fDb.Prepare(
				"DELETE FROM face_rejections WHERE face_id IN"
				" (SELECT id FROM faces WHERE photo_id = ?1)");
			faces.Bind(1, id).Run();
			Statement statement = fDb.Prepare("DELETE FROM faces WHERE photo_id = ?1");
			statement.Bind(1, id).Run();
			statement = fDb.Prepare("DELETE FROM versions WHERE photo_id = ?1");
			statement.Bind(1, id).Run();
			statement = fDb.Prepare("DELETE FROM photos WHERE id = ?1");
			statement.Bind(1, id).Run();
			transaction.Commit();
		}
		_DeleteFiles(photo);
	}
	_Notify(kChangedList | kChangedPeople);
}


void
Library::EmptyTrash()
{
	std::vector<PhotoBrief> photos;
	List(kFilterTrash, 0, photos);
	std::vector<int64_t> ids;
	for (auto& photo : photos)
		ids.push_back(photo.id);
	if (!ids.empty())
		DeletePermanently(ids);
}


void
Library::ExpireTrash(int days)
{
	std::vector<int64_t> ids;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare(
			"SELECT id FROM photos WHERE trashed != 0 AND trashed < ?1");
		statement.Bind(1, NowSeconds() - (int64_t)days * 86400);
		while (statement.Step())
			ids.push_back(statement.Int(0));
	}
	if (!ids.empty())
		DeletePermanently(ids);
}


std::string
Library::OriginalPath(const Photo& photo) const
{
	return photo.path.empty() ? std::string() : JoinPath(fDirectory, photo.path);
}


std::string
Library::DisplayPath(const Photo& photo)
{
	if (photo.version != 0) {
		Version version;
		if (GetVersion(photo.version, version) && !version.path.empty()) {
			std::string path = JoinPath(fDirectory, version.path);
			if (FileExists(path))
				return path;
		}
	}
	return OriginalPath(photo);
}


std::string
Library::ThumbPath(int64_t id, int64_t revision) const
{
	return JoinPath(fDirectory, "thumbs/" + Shard(id) + "/" + std::to_string(id)
		+ "-" + std::to_string(revision) + ".jpg");
}


std::string
Library::PreviewPath(int64_t id, int64_t revision) const
{
	return JoinPath(fDirectory, "previews/" + Shard(id) + "/" + std::to_string(id)
		+ "-" + std::to_string(revision) + ".jpg");
}


std::string
Library::FaceThumbPath(int64_t faceId) const
{
	return JoinPath(fDirectory, "faces/" + Shard(faceId) + "/"
		+ std::to_string(faceId) + ".jpg");
}


void
FaceCropRect(const Face& face, int imageWidth, int imageHeight,
	float& x, float& y, float& w, float& h)
{
	if (imageWidth <= 0 || imageHeight <= 0) {
		x = y = 0;
		w = h = 1;
		return;
	}
	// In pixels: a square 1.7 times the larger side of the box, moved up a
	// little (boxes sit low on the head), shrunk to fit the picture.
	float boxWidth = face.width * imageWidth;
	float boxHeight = face.height * imageHeight;
	float side = std::max(boxWidth, boxHeight) * 1.7f;
	side = std::min(side, (float)std::min(imageWidth, imageHeight));
	side = std::max(side, 1.0f);
	float centerX = (face.x + face.width / 2) * imageWidth;
	float centerY = (face.y + face.height / 2) * imageHeight - boxHeight * 0.08f;
	float left = std::max(0.0f, std::min(imageWidth - side, centerX - side / 2));
	float top = std::max(0.0f, std::min(imageHeight - side, centerY - side / 2));
	x = left / imageWidth;
	y = top / imageHeight;
	w = side / imageWidth;
	h = side / imageHeight;
}


std::string
Library::EditsDirectory(int64_t id) const
{
	return JoinPath(fDirectory, "edits/" + std::to_string(id));
}


void
Library::PruneCaches(int64_t id, int64_t keepRevision)
{
	static const char* const kCaches[] = {"thumbs", "previews", nullptr};
	std::string prefix = std::to_string(id) + "-";
	std::string keep = prefix + std::to_string(keepRevision) + ".jpg";
	for (int i = 0; kCaches[i] != nullptr; i++) {
		std::string directory = JoinPath(fDirectory,
			std::string(kCaches[i]) + "/" + Shard(id));
		std::vector<std::string> names;
		ListDirectory(directory, names);
		for (auto& name : names) {
			if (StartsWith(name, prefix) && name != keep)
				RemoveFile(JoinPath(directory, name));
		}
	}
}


void
Library::_BumpRevision(int64_t id)
{
	Statement statement = fDb.Prepare(
		"UPDATE photos SET revision = revision + 1 WHERE id = ?1");
	statement.Bind(1, id).Run();
}


// --- Versions ---------------------------------------------------------------

void
Library::ListVersions(int64_t photoId, std::vector<Version>& versions)
{
	versions.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(std::string("SELECT ") + kVersionColumns
		+ " FROM versions WHERE photo_id = ?1 ORDER BY created, id");
	statement.Bind(1, photoId);
	while (statement.Step()) {
		versions.emplace_back();
		ReadVersion(statement, versions.back());
	}
}


bool
Library::GetVersion(int64_t versionId, Version& version)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(std::string("SELECT ") + kVersionColumns
		+ " FROM versions WHERE id = ?1");
	statement.Bind(1, versionId);
	if (!statement.Step())
		return false;
	ReadVersion(statement, version);
	return true;
}


int64_t
Library::AddVersion(int64_t photoId, const Version& version,
	const std::string& file, bool move)
{
	int64_t id = 0;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		Statement statement = fDb.Prepare(
			"INSERT INTO versions(photo_id, kind, created, base, params, source,"
			" document, width, height) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
		statement.Bind(1, photoId).Bind(2, version.kind)
			.Bind(3, version.created != 0 ? version.created : NowSeconds())
			.Bind(4, version.base).Bind(5, version.params).Bind(6, version.source)
			.Bind(7, version.document).Bind(8, version.width).Bind(9, version.height);
		if (!statement.Run())
			return 0;
		id = fDb.LastInsertId();

		std::string ext = Extension(file);
		if (ext.empty())
			ext = "jpg";
		std::string relative = "versions/" + std::to_string(photoId) + "/"
			+ std::to_string(id) + "." + ext;
		std::string target = JoinPath(fDirectory, relative);
		MakeDirs(DirName(target));
		bool ok = move ? rename(file.c_str(), target.c_str()) == 0 : false;
		if (!ok) {
			ok = CopyFile(file, target);
			if (ok && move)
				RemoveFile(file);
		}
		if (!ok)
			return 0;	// the transaction rolls back
		statement = fDb.Prepare("UPDATE versions SET path = ?1 WHERE id = ?2");
		statement.Bind(1, relative).Bind(2, id).Run();
		statement = fDb.Prepare(
			"UPDATE photos SET version = ?1, revision = revision + 1 WHERE id = ?2");
		statement.Bind(1, id).Bind(2, photoId).Run();
		transaction.Commit();
	}
	_Notify(kChangedPhoto, photoId);
	return id;
}


bool
Library::UpdateEditVersion(int64_t versionId, const std::string& params,
	const std::string& file, int width, int height)
{
	Version version;
	if (!GetVersion(versionId, version))
		return false;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		std::string target = JoinPath(fDirectory, version.path);
		if (rename(file.c_str(), target.c_str()) != 0) {
			if (!CopyFile(file, target))
				return false;
			RemoveFile(file);
		}
		Transaction transaction(fDb);
		Statement statement = fDb.Prepare(
			"UPDATE versions SET params = ?1, width = ?2, height = ?3, created = ?4"
			" WHERE id = ?5");
		statement.Bind(1, params).Bind(2, width).Bind(3, height)
			.Bind(4, NowSeconds()).Bind(5, versionId).Run();
		statement = fDb.Prepare(
			"UPDATE photos SET version = ?1, revision = revision + 1 WHERE id = ?2");
		statement.Bind(1, versionId).Bind(2, version.photoId).Run();
		transaction.Commit();
	}
	_Notify(kChangedPhoto, version.photoId);
	return true;
}


bool
Library::SetCurrentVersion(int64_t photoId, int64_t versionId)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		if (versionId != 0) {
			Version version;
			if (!GetVersion(versionId, version) || version.photoId != photoId)
				return false;
		}
		Statement statement = fDb.Prepare(
			"UPDATE photos SET version = ?1, revision = revision + 1"
			" WHERE id = ?2 AND version != ?1");
		statement.Bind(1, versionId).Bind(2, photoId).Run();
	}
	_Notify(kChangedPhoto, photoId);
	return true;
}


bool
Library::DeleteVersion(int64_t versionId)
{
	Version version;
	if (!GetVersion(versionId, version))
		return false;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		// Edits made on top of it now start from what it started from.
		Statement statement = fDb.Prepare(
			"UPDATE versions SET base = ?1 WHERE base = ?2");
		statement.Bind(1, version.base).Bind(2, versionId).Run();
		statement = fDb.Prepare("DELETE FROM versions WHERE id = ?1");
		statement.Bind(1, versionId).Run();
		// The photo shows the latest remaining version, else the original.
		statement = fDb.Prepare(
			"UPDATE photos SET revision = revision + 1, version = COALESCE("
			"(SELECT id FROM versions WHERE photo_id = ?1 ORDER BY created DESC,"
			" id DESC LIMIT 1), 0) WHERE id = ?1 AND version = ?2");
		statement.Bind(1, version.photoId).Bind(2, versionId).Run();
		transaction.Commit();
	}
	if (!version.path.empty())
		RemoveFile(JoinPath(fDirectory, version.path));
	if (!version.document.empty())
		RemoveFile(JoinPath(fDirectory, version.document));
	_Notify(kChangedPhoto, version.photoId);
	return true;
}


// --- Faces and people -------------------------------------------------------

void
Library::SetFaces(int64_t photoId, std::vector<Face>& faces, int modelVersion)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		std::vector<Face> old;
		ListFaces(photoId, old);
		for (auto& previous : old)
			RemoveFile(FaceThumbPath(previous.id));
		Transaction transaction(fDb);
		// People named on the old faces stay with the faces in their place.
		for (auto& face : faces) {
			face.photoId = photoId;
			float best = 0.4f;
			for (auto& previous : old) {
				if (previous.personId == 0)
					continue;
				float overlap = Overlap(face, previous);
				if (overlap > best) {
					best = overlap;
					face.personId = previous.personId;
					face.source = previous.source;
				}
			}
		}
		Statement statement = fDb.Prepare(
			"DELETE FROM face_rejections WHERE face_id IN"
			" (SELECT id FROM faces WHERE photo_id = ?1)");
		statement.Bind(1, photoId).Run();
		statement = fDb.Prepare("DELETE FROM faces WHERE photo_id = ?1");
		statement.Bind(1, photoId).Run();
		statement = fDb.Prepare(
			"INSERT INTO faces(photo_id, x, y, w, h, score, quality, person_id,"
			" source, cluster, embedding) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8,"
			" ?9, ?10, ?11)");
		for (auto& face : faces) {
			statement.Reset();
			statement.Bind(1, photoId).Bind(2, (double)face.x).Bind(3, (double)face.y)
				.Bind(4, (double)face.width).Bind(5, (double)face.height)
				.Bind(6, (double)face.score).Bind(7, (double)face.quality)
				.Bind(8, face.personId).Bind(9, face.source).Bind(10, face.cluster)
				.BindBlob(11, face.embedding.data(),
					face.embedding.size() * sizeof(float));
			if (statement.Run())
				face.id = fDb.LastInsertId();
		}
		statement = fDb.Prepare("UPDATE photos SET faces_version = ?1 WHERE id = ?2");
		statement.Bind(1, modelVersion).Bind(2, photoId).Run();
		transaction.Commit();
	}
	if (!faces.empty())
		_Notify(kChangedPeople, photoId);
}


void
Library::ListFaces(int64_t photoId, std::vector<Face>& faces, bool withEmbeddings)
{
	faces.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(std::string("SELECT ") + kFaceColumns
		+ (withEmbeddings ? ", embedding" : "")
		+ " FROM faces WHERE photo_id = ?1 ORDER BY x");
	statement.Bind(1, photoId);
	while (statement.Step()) {
		faces.emplace_back();
		ReadFace(statement, faces.back(), withEmbeddings ? 11 : -1);
	}
}


void
Library::ListAllFaces(std::vector<Face>& faces, bool withEmbeddings)
{
	faces.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(std::string("SELECT f.id, f.photo_id, f.x,"
		" f.y, f.w, f.h, f.score, f.quality, f.person_id, f.source, f.cluster")
		+ (withEmbeddings ? ", f.embedding" : "")
		+ " FROM faces f JOIN photos p ON p.id = f.photo_id WHERE p.trashed = 0"
		" ORDER BY f.id");
	while (statement.Step()) {
		faces.emplace_back();
		ReadFace(statement, faces.back(), withEmbeddings ? 11 : -1);
	}
}


bool
Library::GetFace(int64_t faceId, Face& face, bool withEmbedding)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(std::string("SELECT ") + kFaceColumns
		+ (withEmbedding ? ", embedding" : "") + " FROM faces WHERE id = ?1");
	statement.Bind(1, faceId);
	if (!statement.Step())
		return false;
	ReadFace(statement, face, withEmbedding ? 11 : -1);
	return true;
}


void
Library::ListPeople(std::vector<Person>& people, bool withHidden)
{
	people.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"SELECT pe.id, pe.name, pe.cover_face, pe.hidden,"
		" COUNT(f.id), COUNT(DISTINCT f.photo_id)"
		" FROM people pe"
		" LEFT JOIN (SELECT f.id, f.photo_id, f.person_id FROM faces f"
		"  JOIN photos p ON p.id = f.photo_id AND p.trashed = 0) f"
		"  ON f.person_id = pe.id"
		" GROUP BY pe.id ORDER BY COUNT(f.id) DESC, pe.name COLLATE NOCASE");
	while (statement.Step()) {
		Person person;
		person.id = statement.Int(0);
		person.name = statement.Text(1);
		person.coverFace = statement.Int(2);
		person.hidden = statement.Int(3) != 0;
		person.faceCount = (int)statement.Int(4);
		person.photoCount = (int)statement.Int(5);
		if (person.hidden && !withHidden)
			continue;
		people.push_back(person);
	}
}


bool
Library::GetPerson(int64_t id, Person& person)
{
	std::vector<Person> people;
	ListPeople(people, true);
	for (auto& candidate : people) {
		if (candidate.id == id) {
			person = candidate;
			return true;
		}
	}
	return false;
}


int64_t
Library::FindPerson(const std::string& name)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"SELECT id FROM people WHERE name = ?1 COLLATE NOCASE");
	statement.Bind(1, Trim(name));
	return statement.Step() ? statement.Int(0) : 0;
}


int64_t
Library::CreatePerson(const std::string& name)
{
	int64_t id = 0;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare(
			"INSERT INTO people(name, created) VALUES(?1, ?2)");
		statement.Bind(1, Trim(name)).Bind(2, NowSeconds());
		if (statement.Run())
			id = fDb.LastInsertId();
	}
	_Notify(kChangedPeople);
	return id;
}


void
Library::RenamePerson(int64_t id, const std::string& name)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare("UPDATE people SET name = ?1 WHERE id = ?2");
		statement.Bind(1, Trim(name)).Bind(2, id).Run();
	}
	_Notify(kChangedPeople);
}


void
Library::SetPersonHidden(int64_t id, bool hidden)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare("UPDATE people SET hidden = ?1 WHERE id = ?2");
		statement.Bind(1, hidden ? 1 : 0).Bind(2, id).Run();
	}
	_Notify(kChangedPeople);
}


void
Library::SetPersonCover(int64_t id, int64_t faceId)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare(
			"UPDATE people SET cover_face = ?1 WHERE id = ?2");
		statement.Bind(1, faceId).Bind(2, id).Run();
	}
	_Notify(kChangedPeople);
}


void
Library::MergePeople(int64_t from, int64_t into)
{
	if (from == into || from == 0 || into == 0)
		return;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		Statement statement = fDb.Prepare(
			"UPDATE faces SET person_id = ?1 WHERE person_id = ?2");
		statement.Bind(1, into).Bind(2, from).Run();
		statement = fDb.Prepare(
			"UPDATE OR IGNORE face_rejections SET person_id = ?1 WHERE person_id = ?2");
		statement.Bind(1, into).Bind(2, from).Run();
		statement = fDb.Prepare("DELETE FROM face_rejections WHERE person_id = ?1");
		statement.Bind(1, from).Run();
		statement = fDb.Prepare("DELETE FROM people WHERE id = ?1");
		statement.Bind(1, from).Run();
		transaction.Commit();
	}
	_Notify(kChangedPeople);
}


void
Library::DeletePerson(int64_t id)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		Statement statement = fDb.Prepare(
			"UPDATE faces SET person_id = 0, source = 0 WHERE person_id = ?1");
		statement.Bind(1, id).Run();
		statement = fDb.Prepare("DELETE FROM face_rejections WHERE person_id = ?1");
		statement.Bind(1, id).Run();
		statement = fDb.Prepare("DELETE FROM people WHERE id = ?1");
		statement.Bind(1, id).Run();
		transaction.Commit();
	}
	_Notify(kChangedPeople);
}


void
Library::AssignFace(int64_t faceId, int64_t personId, int source)
{
	std::vector<int64_t> ids(1, faceId);
	AssignFaces(ids, personId, source);
}


void
Library::AssignFaces(const std::vector<int64_t>& faceIds, int64_t personId, int source)
{
	if (faceIds.empty())
		return;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		Statement statement = fDb.Prepare(
			"UPDATE faces SET person_id = ?1, source = ?2 WHERE id = ?3");
		Statement forgive = fDb.Prepare(
			"DELETE FROM face_rejections WHERE face_id = ?1 AND person_id = ?2");
		for (int64_t faceId : faceIds) {
			statement.Reset();
			statement.Bind(1, personId)
				.Bind(2, personId == 0 ? (int)kFaceUnassigned : source)
				.Bind(3, faceId).Run();
			if (personId != 0 && source == kFaceUser) {
				forgive.Reset();
				forgive.Bind(1, faceId).Bind(2, personId).Run();
			}
		}
		// A person without a cover takes the first face.
		if (personId != 0) {
			statement = fDb.Prepare(
				"UPDATE people SET cover_face = ?1 WHERE id = ?2 AND (cover_face = 0"
				" OR cover_face NOT IN (SELECT id FROM faces WHERE person_id = ?2))");
			statement.Bind(1, faceIds[0]).Bind(2, personId).Run();
		}
		transaction.Commit();
	}
	_Notify(kChangedPeople);
}


void
Library::SetFaceCluster(int64_t faceId, int64_t cluster)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare("UPDATE faces SET cluster = ?1 WHERE id = ?2");
	statement.Bind(1, cluster).Bind(2, faceId).Run();
}


void
Library::SetFaceClusters(const std::vector<std::pair<int64_t, int64_t>>& faceClusters)
{
	if (faceClusters.empty())
		return;
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Transaction transaction(fDb);
	Statement statement = fDb.Prepare("UPDATE faces SET cluster = ?1 WHERE id = ?2");
	for (auto& pair : faceClusters) {
		statement.Reset();
		statement.Bind(1, pair.second).Bind(2, pair.first).Run();
	}
	transaction.Commit();
}


void
Library::RejectFace(int64_t faceId, int64_t personId)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		Statement statement = fDb.Prepare(
			"INSERT OR IGNORE INTO face_rejections(face_id, person_id) VALUES(?1, ?2)");
		statement.Bind(1, faceId).Bind(2, personId).Run();
		statement = fDb.Prepare(
			"UPDATE faces SET person_id = 0, source = 0"
			" WHERE id = ?1 AND person_id = ?2");
		statement.Bind(1, faceId).Bind(2, personId).Run();
		transaction.Commit();
	}
	_Notify(kChangedPeople);
}


bool
Library::IsRejected(int64_t faceId, int64_t personId)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"SELECT 1 FROM face_rejections WHERE face_id = ?1 AND person_id = ?2");
	statement.Bind(1, faceId).Bind(2, personId);
	return statement.Step();
}


void
Library::ListRejections(std::vector<std::pair<int64_t, int64_t>>& pairs)
{
	pairs.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare("SELECT face_id, person_id FROM face_rejections");
	while (statement.Step())
		pairs.emplace_back(statement.Int(0), statement.Int(1));
}


void
Library::DeleteFace(int64_t faceId)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Transaction transaction(fDb);
		RemoveFile(FaceThumbPath(faceId));
		Statement statement = fDb.Prepare(
			"DELETE FROM face_rejections WHERE face_id = ?1");
		statement.Bind(1, faceId).Run();
		statement = fDb.Prepare("DELETE FROM faces WHERE id = ?1");
		statement.Bind(1, faceId).Run();
		transaction.Commit();
	}
	_Notify(kChangedPeople);
}


// --- Immich -----------------------------------------------------------------

int64_t
Library::ApplyRemoteAsset(const RemoteAsset& asset, bool* added)
{
	if (added != nullptr)
		*added = false;
	if (asset.id.empty() || !asset.isImage)
		return 0;
	int64_t id = 0;
	uint32_t changes = 0;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Photo photo;
		if (GetByImmichId(asset.id, photo)) {
			id = photo.id;
			// The server's word on what both sides can change, unless a
			// change made here is still waiting to be sent.
			Statement statement = fDb.Prepare(
				"UPDATE photos SET"
				" favorite = CASE WHEN dirty & 1 THEN favorite ELSE ?1 END,"
				" thumbhash = ?2,"
				" taken = CASE WHEN path = '' AND ?3 != 0 THEN ?3 ELSE taken END,"
				" width = CASE WHEN path = '' AND ?4 > 0 THEN ?4 ELSE width END,"
				" height = CASE WHEN path = '' AND ?5 > 0 THEN ?5 ELSE height END"
				" WHERE id = ?6");
			statement.Bind(1, asset.favorite ? 1 : 0).Bind(2, asset.thumbhash)
				.Bind(3, asset.taken).Bind(4, asset.width).Bind(5, asset.height)
				.Bind(6, id).Run();
			changes |= kChangedPhoto;
			// In the server's trash: in the trash here; and back.
			if (asset.trashed && photo.trashed == 0) {
				statement = fDb.Prepare("UPDATE photos SET trashed = ?1 WHERE id = ?2");
				statement.Bind(1, NowSeconds()).Bind(2, id).Run();
				changes |= kChangedList;
			} else if (!asset.trashed && photo.trashed != 0 && photo.path.empty()) {
				statement = fDb.Prepare("UPDATE photos SET trashed = 0 WHERE id = ?1");
				statement.Bind(1, id).Run();
				changes |= kChangedList;
			}
		} else {
			Statement removed = fDb.Prepare(
				"SELECT 1 FROM removed_remote WHERE immich_id = ?1");
			removed.Bind(1, asset.id);
			if (removed.Step())
				return 0;
			// A picture that is here already (imported from the same card,
			// say) is the same photo.
			bool linked = false;
			if (!asset.checksum.empty()) {
				Statement statement = fDb.Prepare(
					"SELECT id FROM photos WHERE sha1 = ?1 AND immich_id IS NULL"
					" ORDER BY trashed != 0, id LIMIT 1");
				statement.Bind(1, asset.checksum);
				if (statement.Step()) {
					id = statement.Int(0);
					linked = true;
				}
			}
			if (linked) {
				Statement statement = fDb.Prepare(
					"UPDATE photos SET immich_id = ?1, immich_state = ?2,"
					" thumbhash = ?3 WHERE id = ?4");
				statement.Bind(1, asset.id).Bind(2, (int)kImmichSynced)
					.Bind(3, asset.thumbhash).Bind(4, id).Run();
				changes |= kChangedPhoto;
			} else {
				if (asset.trashed)
					return 0;
				Statement statement = fDb.Prepare(
					"INSERT INTO photos(name, kind, sha1, width, height, taken,"
					" imported, favorite, immich_id, immich_state, thumbhash)"
					" VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
				statement.Bind(1, asset.name).Bind(2, (int)KindOfName(asset.name))
					.Bind(3, asset.checksum).Bind(4, asset.width).Bind(5, asset.height)
					.Bind(6, asset.taken)
					.Bind(7, asset.uploaded != 0 ? asset.uploaded : NowSeconds())
					.Bind(8, asset.favorite ? 1 : 0).Bind(9, asset.id)
					.Bind(10, (int)kImmichRemoteOnly).Bind(11, asset.thumbhash);
				if (!statement.Run())
					return 0;
				id = fDb.LastInsertId();
				if (added != nullptr)
					*added = true;
				changes |= kChangedList;
			}
		}
	}
	// The caller batches thousands of these: it tells the listener itself
	// when added is asked for.
	if (added == nullptr && changes != 0)
		_Notify(changes, id);
	return id;
}


void
Library::ApplyRemoteExif(const RemoteExif& exif)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	// Only for photos whose original is not here: a local file speaks for
	// itself.
	Statement statement = fDb.Prepare(
		"UPDATE photos SET"
		" size = CASE WHEN ?1 > 0 THEN ?1 ELSE size END,"
		" make = ?2, model = ?3, lens = ?4, iso = ?5, exposure = ?6,"
		" fnumber = ?7, focal = ?8, has_gps = ?9, latitude = ?10, longitude = ?11,"
		" width = CASE WHEN width = 0 AND ?12 > 0 THEN ?12 ELSE width END,"
		" height = CASE WHEN height = 0 AND ?13 > 0 THEN ?13 ELSE height END"
		" WHERE immich_id = ?14 AND path = ''");
	bool turned = exif.orientation >= 5;
	statement.Bind(1, exif.size).Bind(2, exif.make).Bind(3, exif.model)
		.Bind(4, exif.lens).Bind(5, exif.iso).Bind(6, exif.exposureTime)
		.Bind(7, exif.fNumber).Bind(8, exif.focalLength)
		.Bind(9, exif.hasGps ? 1 : 0).Bind(10, exif.latitude).Bind(11, exif.longitude)
		.Bind(12, turned ? exif.height : exif.width)
		.Bind(13, turned ? exif.width : exif.height).Bind(14, exif.assetId).Run();
}


void
Library::RemoveRemoteAsset(const std::string& immichId)
{
	Photo photo;
	bool changed = false;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement forget = fDb.Prepare(
			"DELETE FROM removed_remote WHERE immich_id = ?1");
		forget.Bind(1, immichId).Run();
		if (!GetByImmichId(immichId, photo))
			return;
		if (!photo.path.empty()) {
			// The original is here: the photo goes to the trash, where it can
			// be kept (restoring it uploads it again).
			Statement statement = fDb.Prepare(
				"UPDATE photos SET immich_id = NULL, immich_state = 0,"
				" trashed = CASE WHEN trashed = 0 THEN ?1 ELSE trashed END"
				" WHERE id = ?2");
			statement.Bind(1, NowSeconds()).Bind(2, photo.id).Run();
			changed = true;
		}
	}
	if (changed) {
		_Notify(kChangedList | kChangedPeople);
		return;
	}
	// Nothing of it is here: forget it. Not remembered in removed_remote
	// (the server deleted it, there is nothing to hold back).
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare(
			"UPDATE photos SET immich_id = NULL WHERE id = ?1");
		statement.Bind(1, photo.id).Run();
	}
	std::vector<int64_t> ids(1, photo.id);
	DeletePermanently(ids);
}


void
Library::SetImmichLink(int64_t id, const std::string& immichId, int state)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare(
			"UPDATE photos SET immich_id = ?1, immich_state = ?2 WHERE id = ?3");
		if (immichId.empty())
			statement.BindNull(1);
		else
			statement.Bind(1, immichId);
		statement.Bind(2, state).Bind(3, id).Run();
	}
	_Notify(kChangedPhoto, id);
}


bool
Library::DropOriginal(int64_t id)
{
	Photo photo;
	if (!Get(id, photo) || photo.path.empty() || photo.immichId.empty()
		|| photo.version != 0)
		return false;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare(
			"UPDATE photos SET path = '', immich_state = ?1 WHERE id = ?2");
		statement.Bind(1, (int)kImmichRemoteOnly).Bind(2, id).Run();
	}
	RemoveFile(JoinPath(fDirectory, photo.path));
	_Notify(kChangedPhoto, id);
	return true;
}


void
Library::ListForUpload(std::vector<int64_t>& ids)
{
	ids.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"SELECT id FROM photos WHERE path != '' AND immich_id IS NULL"
		" AND immich_state = 0 AND trashed = 0 ORDER BY taken, id");
	while (statement.Step())
		ids.push_back(statement.Int(0));
}


void
Library::ListPendingFavorites(std::vector<PendingFavorite>& pending)
{
	pending.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"SELECT id, immich_id, favorite FROM photos"
		" WHERE dirty & 1 AND immich_id IS NOT NULL");
	while (statement.Step()) {
		PendingFavorite item;
		item.id = statement.Int(0);
		item.immichId = statement.Text(1);
		item.favorite = statement.Int(2) != 0;
		pending.push_back(item);
	}
}


void
Library::ClearPendingFavorite(int64_t id)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"UPDATE photos SET dirty = dirty & ~1 WHERE id = ?1");
	statement.Bind(1, id).Run();
}


void
Library::ListRemovedRemote(std::vector<std::string>& immichIds)
{
	immichIds.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"SELECT immich_id FROM removed_remote WHERE pushed = 0");
	while (statement.Step())
		immichIds.push_back(statement.Text(0));
}


void
Library::MarkRemovedRemote(const std::vector<std::string>& immichIds, int state)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Transaction transaction(fDb);
	Statement statement = fDb.Prepare(
		"UPDATE removed_remote SET pushed = ?1 WHERE immich_id = ?2");
	for (auto& id : immichIds) {
		statement.Reset();
		statement.Bind(1, state).Bind(2, id).Run();
	}
	transaction.Commit();
}


void
Library::ListRemoteOnly(std::vector<PhotoBrief>& photos)
{
	photos.clear();
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(std::string("SELECT ") + kBriefColumns
		+ " FROM photos WHERE path = '' AND immich_id IS NOT NULL AND trashed = 0"
		" ORDER BY taken DESC, id DESC");
	while (statement.Step()) {
		photos.emplace_back();
		ReadBrief(statement, photos.back());
	}
}


void
Library::ForgetImmich()
{
	std::vector<int64_t> remoteOnly;
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		Statement statement = fDb.Prepare("SELECT id FROM photos WHERE path = ''");
		while (statement.Step())
			remoteOnly.push_back(statement.Int(0));
		fDb.Exec("UPDATE photos SET immich_id = NULL, immich_state = 0, dirty = 0");
	}
	if (!remoteOnly.empty())
		DeletePermanently(remoteOnly);
	{
		std::lock_guard<std::recursive_mutex> lock(fLock);
		fDb.Exec("DELETE FROM removed_remote");
	}
	_Notify(kChangedList | kChangedSync);
}


// --- Settings ---------------------------------------------------------------

std::string
Library::Setting(const std::string& key, const std::string& fallback)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare("SELECT value FROM settings WHERE key = ?1");
	statement.Bind(1, key);
	return statement.Step() ? statement.Text(0) : fallback;
}


void
Library::SetSetting(const std::string& key, const std::string& value)
{
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Statement statement = fDb.Prepare(
		"INSERT INTO settings(key, value) VALUES(?1, ?2)"
		" ON CONFLICT(key) DO UPDATE SET value = excluded.value");
	statement.Bind(1, key).Bind(2, value).Run();
}

}	// namespace photos
