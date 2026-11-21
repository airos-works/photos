// End-to-end sync against tools/fake-immich.py (running; tools/test-sync.sh
// starts it): the real client and engine over plain HTTP, a library in a
// temporary directory.
//
//   PHOTOS_TEST_SERVER    http://127.0.0.1:18283
//   PHOTOS_TEST_FIXTURES  a directory made by tools/make-fixtures.py
//   PHOTOS_TEST_TMP       a scratch directory (emptied)
#include "../Test.h"
#include "PlainHttp.h"

#include "core/Library.h"
#include "nlohmann/json.hpp"
#include "sync/SyncEngine.h"

#include <cstdlib>

using namespace photos;
using nlohmann::json;

static std::string sServer;
static std::string sFixtures;
static std::string sTmp;
static PlainHttp sHttp;


static json
Admin(const std::string& path, const std::string& method = "POST")
{
	HttpRequest request;
	request.method = method;
	request.url = sServer + path;
	HttpResponse response = sHttp.Perform(request, nullptr);
	return json::parse(response.body, nullptr, false);
}


static json
ServerAssets()
{
	return Admin("/admin/assets", "GET");
}


static json
ServerAsset(const std::string& id)
{
	for (auto& asset : ServerAssets()) {
		if (asset["id"] == id)
			return asset;
	}
	return json();
}


struct Fixture {
	Library library;
	SyncEngine engine;
	std::string token;

	explicit Fixture(const std::string& name)
		:
		engine(library, sHttp)
	{
		std::string directory = sTmp + "/" + name;
		RemoveTree(directory);
		std::string error;
		if (!library.Open(directory, &error))
			std::cerr << "library: " << error << "\n";
		ImmichClient client(sHttp);
		client.SetServer(sServer);
		if (!client.Login("test@example.com", "secret", error))
			std::cerr << "login: " << error << "\n";
		token = client.Token();
		SyncOptions options;
		options.server = sServer;
		options.token = token;
		engine.SetOptions(options);
	}

	SyncResult Sync()
	{
		SyncResult result = engine.Run(nullptr, nullptr);
		if (!result.ok)
			std::cerr << "sync: " << result.error << "\n";
		return result;
	}

	int Count(PhotoFilter filter = kFilterAll) { return library.Count(filter); }
};


TEST(SignInIsChecked)
{
	ImmichClient client(sHttp);
	client.SetServer(sServer + "/api/");
	CHECK_EQ(client.Server(), sServer);
	std::string error;
	CHECK(!client.Login("test@example.com", "wrong", error));
	CHECK(!error.empty());
	CHECK(client.Login("test@example.com", "secret", error));
	CHECK(client.Validate(error));
	CHECK_EQ(client.UserName(), std::string("Test User"));
	CHECK_EQ(client.MajorVersion(), 3);
	client.SetToken("nonsense");
	CHECK(!client.Validate(error));
	CHECK(client.Unauthorized());
}


TEST(PullListsWhatTheServerHas)
{
	Admin("/admin/reset");
	json seeded = Admin("/admin/seed?dir=" + sFixtures + "/DCIM");
	int count = seeded["added"];
	CHECK(count >= 10);

	Fixture a("pull");
	SyncResult result = a.Sync();
	CHECK(result.ok);
	CHECK_EQ(result.added, count);
	CHECK_EQ(a.Count(), count);
	std::vector<PhotoBrief> remote;
	a.library.ListRemoteOnly(remote);
	CHECK_EQ((int)remote.size(), count);

	// Nothing new: nothing happens, nothing is uploaded (nothing is local).
	result = a.Sync();
	CHECK(result.ok);
	CHECK_EQ(result.added, 0);
	CHECK_EQ(result.uploaded, 0);
	CHECK_EQ((int)ServerAssets().size(), count);

	// A photo's details came with it.
	Photo photo;
	CHECK(a.library.Get(remote[0].id, photo));
	CHECK(!photo.immichId.empty());
	CHECK_EQ(photo.immichState, (int)kImmichRemoteOnly);
	CHECK(photo.taken != 0);
	CHECK(photo.size > 0);
	CHECK_EQ(photo.make, std::string("Fake"));
	CHECK(photo.width > 0);

	// Its thumbnail and its original on demand.
	std::string error;
	std::string thumb = a.library.ThumbPath(photo.id, photo.revision);
	CHECK(a.engine.FetchThumbnail(photo.immichId, thumb, error));
	CHECK(FileSize(thumb) > 100);
	CHECK(a.engine.FetchOriginal(photo.id, error));
	CHECK(a.library.Get(photo.id, photo));
	CHECK(!photo.path.empty());
	CHECK_EQ(photo.immichState, (int)kImmichSynced);
	CHECK_EQ(Sha1OfFile(a.library.OriginalPath(photo)), photo.sha1);
	CHECK_EQ(a.Count(), count);
	// And it can be dropped again to save the space.
	CHECK(a.library.DropOriginal(photo.id));
	CHECK(a.library.Get(photo.id, photo));
	CHECK(photo.path.empty());
}


TEST(PushSendsWhatIsNotThere)
{
	Admin("/admin/reset");
	Admin("/admin/seed?dir=" + sFixtures + "/DCIM");
	int onServer = (int)ServerAssets().size();

	Fixture a("push");
	CHECK(a.Sync().ok);
	// A picture the server does not have, and one it has.
	ImportOptions options;
	ImportResult fresh = a.library.ImportFile(sFixtures + "/screenshot one.png", options);
	CHECK(fresh.added);
	ImportResult known = a.library.ImportFile(sFixtures + "/DCIM/100TEST/IMG_0003.JPG",
		options);
	// The one the server has was listed already: the file is its original.
	CHECK(!known.added);
	CHECK(known.id != 0);
	CHECK_EQ(a.Count(), onServer + 1);
	Photo photo;
	CHECK(a.library.Get(known.id, photo));
	CHECK_EQ(photo.immichState, (int)kImmichSynced);
	CHECK(!photo.path.empty());

	SyncResult result = a.Sync();
	CHECK(result.ok);
	CHECK_EQ(result.uploaded, 1);
	CHECK_EQ(result.failed, 0);
	CHECK_EQ((int)ServerAssets().size(), onServer + 1);
	CHECK(a.library.Get(fresh.id, photo));
	CHECK(!photo.immichId.empty());
	CHECK_EQ(photo.immichState, (int)kImmichSynced);
	json uploaded = ServerAsset(photo.immichId);
	CHECK_EQ(uploaded["name"].get<std::string>(), std::string("screenshot one.png"));
	CHECK_EQ(uploaded["checksum"].get<std::string>(), photo.sha1);

	// Again: nothing more to send, and the server's echo of the upload does
	// not make a second photo.
	result = a.Sync();
	CHECK_EQ(result.uploaded, 0);
	CHECK_EQ(result.added, 0);
	CHECK_EQ(a.Count(), onServer + 1);

	// Without uploading nothing is sent.
	ImportResult held = a.library.ImportFile(sFixtures + "/picture.webp", options);
	CHECK(held.added);
	SyncOptions off = a.engine.Options();
	off.upload = false;
	a.engine.SetOptions(off);
	CHECK_EQ(a.Sync().uploaded, 0);
	CHECK_EQ((int)ServerAssets().size(), onServer + 1);

	// A second library on the same account sees the upload.
	Fixture b("push-b");
	CHECK(b.Sync().ok);
	CHECK_EQ(b.Count(), onServer + 1);
}


TEST(SameFileOnBothSidesIsOnePhoto)
{
	Admin("/admin/reset");
	Fixture a("same");
	ImportOptions options;
	ImportResult local = a.library.ImportFile(sFixtures + "/DCIM/100TEST/IMG_0005.JPG",
		options);
	CHECK(local.added);
	// The server gets the same file from elsewhere before this library syncs.
	Admin("/admin/seed?dir=" + sFixtures + "/DCIM");
	int onServer = (int)ServerAssets().size();
	SyncResult result = a.Sync();
	CHECK(result.ok);
	CHECK_EQ(result.uploaded, 0);
	CHECK_EQ(a.Count(), onServer);
	Photo photo;
	CHECK(a.library.Get(local.id, photo));
	CHECK(!photo.immichId.empty());
	CHECK_EQ(photo.immichState, (int)kImmichSynced);
	CHECK_EQ((int)ServerAssets().size(), onServer);
}


TEST(FavoritesGoBothWays)
{
	Admin("/admin/reset");
	Admin("/admin/seed?dir=" + sFixtures + "/DCIM");
	Fixture a("favorites");
	CHECK(a.Sync().ok);
	std::vector<PhotoBrief> photos;
	a.library.List(kFilterAll, 0, photos);
	Photo first, second;
	CHECK(a.library.Get(photos[0].id, first));
	CHECK(a.library.Get(photos[1].id, second));

	a.library.SetFavorite(first.id, true);
	CHECK(a.Sync().ok);
	CHECK(ServerAsset(first.immichId)["favorite"].get<bool>());

	Admin("/admin/favorite?id=" + second.immichId + "&on=1");
	CHECK(a.Sync().ok);
	CHECK(a.library.Get(second.id, second));
	CHECK(second.favorite);
	CHECK(a.library.Get(first.id, first));
	CHECK(first.favorite);
	CHECK_EQ(a.Count(kFilterFavorites), 2);

	Admin("/admin/favorite?id=" + first.immichId + "&on=0");
	CHECK(a.Sync().ok);
	CHECK(a.library.Get(first.id, first));
	CHECK(!first.favorite);
}


TEST(DeletionsOnTheServer)
{
	Admin("/admin/reset");
	Admin("/admin/seed?dir=" + sFixtures + "/DCIM");
	Fixture a("server-deletes");
	CHECK(a.Sync().ok);
	int count = a.Count();
	std::vector<PhotoBrief> photos;
	a.library.List(kFilterAll, 0, photos);
	Photo remoteOnly, withOriginal, trashed;
	CHECK(a.library.Get(photos[0].id, remoteOnly));
	CHECK(a.library.Get(photos[1].id, withOriginal));
	CHECK(a.library.Get(photos[2].id, trashed));
	std::string error;
	CHECK(a.engine.FetchOriginal(withOriginal.id, error));

	Admin("/admin/delete?id=" + remoteOnly.immichId);
	Admin("/admin/delete?id=" + withOriginal.immichId);
	Admin("/admin/trash?id=" + trashed.immichId);
	SyncOptions options = a.engine.Options();
	options.upload = false;		// else the kept original would go right back
	a.engine.SetOptions(options);
	SyncResult result = a.Sync();
	CHECK(result.ok);
	CHECK_EQ(result.removed, 2);
	// Nothing of the first was here: it is gone.
	Photo photo;
	CHECK(!a.library.Get(remoteOnly.id, photo));
	// The second has its original here: in the trash, not linked any more.
	CHECK(a.library.Get(withOriginal.id, photo));
	CHECK(photo.trashed != 0);
	CHECK(photo.immichId.empty());
	CHECK(FileExists(a.library.OriginalPath(photo)));
	// The third is in the server's trash: in the trash here.
	CHECK(a.library.Get(trashed.id, photo));
	CHECK(photo.trashed != 0);
	CHECK_EQ(a.Count(), count - 3);
	CHECK_EQ(a.Count(kFilterTrash), 2);
}


TEST(DeletionsHere)
{
	Admin("/admin/reset");
	Admin("/admin/seed?dir=" + sFixtures + "/DCIM");
	int onServer = (int)ServerAssets().size();
	Fixture a("local-deletes");
	CHECK(a.Sync().ok);
	std::vector<PhotoBrief> photos;
	a.library.List(kFilterAll, 0, photos);
	Photo kept, pushed;
	CHECK(a.library.Get(photos[0].id, kept));
	CHECK(a.library.Get(photos[1].id, pushed));

	// Deleted here for good, the server is left alone: the photo stays
	// there and does not come back here.
	std::vector<int64_t> ids(1, kept.id);
	a.library.DeletePermanently(ids);
	CHECK(a.Sync().ok);
	CHECK_EQ((int)ServerAssets().size(), onServer);
	CHECK(ServerAsset(kept.immichId)["trashed"].is_null());
	CHECK_EQ(a.Count(), onServer - 1);
	Admin("/admin/favorite?id=" + kept.immichId + "&on=1");	// it changes there
	CHECK(a.Sync().ok);
	CHECK_EQ(a.Count(), onServer - 1);

	// With the option the server's copy goes to its trash.
	SyncOptions options = a.engine.Options();
	options.trashOnServer = true;
	a.engine.SetOptions(options);
	ids[0] = pushed.id;
	a.library.DeletePermanently(ids);
	CHECK(a.Sync().ok);
	CHECK(!ServerAsset(pushed.immichId)["trashed"].is_null());
	// The earlier one, decided when the option was off, is not touched.
	CHECK(ServerAsset(kept.immichId)["trashed"].is_null());
	CHECK_EQ(a.Count(), onServer - 2);
}


TEST(ServerAsksForAFreshStart)
{
	Admin("/admin/reset");
	Admin("/admin/seed?dir=" + sFixtures + "/DCIM");
	Fixture a("reset");
	CHECK(a.Sync().ok);
	int count = a.Count();
	std::vector<PhotoBrief> photos;
	a.library.List(kFilterAll, 0, photos);
	Photo gone;
	CHECK(a.library.Get(photos[3].id, gone));
	// Deleted while the server also forgets what this session has seen.
	Admin("/admin/delete?id=" + gone.immichId);
	Admin("/admin/sync-reset");
	SyncResult result = a.Sync();
	CHECK(result.ok);
	CHECK_EQ(result.added, 0);
	CHECK_EQ(a.Count(), count - 1);
	Photo photo;
	CHECK(!a.library.Get(gone.id, photo));
}


TEST(AnotherAccountStartsOver)
{
	Admin("/admin/reset");
	Admin("/admin/seed?dir=" + sFixtures + "/DCIM");
	Fixture a("account");
	ImportOptions options;
	ImportResult local = a.library.ImportFile(sFixtures + "/scan.tif", options);
	CHECK(local.added);
	SyncOptions off = a.engine.Options();
	off.upload = false;
	a.engine.SetOptions(off);
	CHECK(a.Sync().ok);
	int count = a.Count();
	CHECK(count > 1);
	// The same library pointed at "another server" (the identity differs).
	a.library.SetSetting("immich.identity", "http://elsewhere|someone");
	CHECK(a.Sync().ok);
	// What was only on the old server is replaced by the new listing; the
	// local photo stays.
	CHECK_EQ(a.Count(), count);
	Photo photo;
	CHECK(a.library.Get(local.id, photo));
	CHECK(!photo.path.empty());
}


int
main(int argc, char** argv)
{
	sServer = getenv("PHOTOS_TEST_SERVER") != nullptr ? getenv("PHOTOS_TEST_SERVER")
		: "http://127.0.0.1:18283";
	sFixtures = getenv("PHOTOS_TEST_FIXTURES") != nullptr
		? getenv("PHOTOS_TEST_FIXTURES") : "";
	sTmp = getenv("PHOTOS_TEST_TMP") != nullptr ? getenv("PHOTOS_TEST_TMP") : "";
	if (sFixtures.empty() || sTmp.empty()) {
		std::cerr << "set PHOTOS_TEST_FIXTURES and PHOTOS_TEST_TMP (tools/test-sync.sh)\n";
		return 2;
	}
	MakeDirs(sTmp);
	return test::RunAll(argc > 1 ? argv[1] : nullptr);
}
