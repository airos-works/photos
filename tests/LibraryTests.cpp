#include "Test.h"

#include "core/Library.h"
#include "core/People.h"

#include <cmath>
#include <cstdlib>
#include <random>

using namespace photos;

namespace {

std::string
Scratch(const char* name)
{
	std::string base = getenv("PHOTOS_TEST_TMP") != nullptr ? getenv("PHOTOS_TEST_TMP")
		: "build-host/test-tmp";
	std::string directory = base + "/" + name;
	RemoveTree(directory);
	MakeDirs(directory);
	return directory;
}


// A file that passes for a JPEG as far as the library looks: a frame header
// with a size, and a tail that makes it unique.
std::string
FakeJpeg(const std::string& directory, const std::string& name, int width,
	int height, const std::string& salt)
{
	std::string data("\xff\xd8", 2);
	data += std::string("\xff\xc0\x00\x11\x08", 5);
	data.push_back((char)(height >> 8));
	data.push_back((char)height);
	data.push_back((char)(width >> 8));
	data.push_back((char)width);
	data += std::string("\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01", 10);
	data += "\xff\xfe";
	data.push_back((char)((salt.size() + 2) >> 8));
	data.push_back((char)(salt.size() + 2));
	data += salt;
	data += "\xff\xd9";
	std::string path = JoinPath(directory, name);
	WriteFile(path, data.data(), data.size());
	return path;
}

}	// namespace


TEST(LibraryImportAndDuplicates)
{
	std::string scratch = Scratch("import");
	Library library;
	std::string error;
	CHECK(library.Open(scratch + "/lib", &error));
	int changes = 0;
	library.SetListener([&](uint32_t, int64_t) { changes++; });

	std::string a = FakeJpeg(scratch, "IMG_0001.JPG", 640, 480, "one");
	std::string b = FakeJpeg(scratch, "IMG_0002.JPG", 480, 640, "two");
	ImportOptions options;
	options.importId = library.BeginImport("Test");
	options.takenFallback = 1700000000;
	ImportResult first = library.ImportFile(a, options);
	CHECK(first.added);
	CHECK(first.id != 0);
	CHECK(changes > 0);
	ImportResult second = library.ImportFile(b, options);
	CHECK(second.added);
	// The same bytes again, under another name: the same photo.
	std::string copy = scratch + "/copy.jpg";
	CHECK(CopyFile(a, copy));
	ImportResult again = library.ImportFile(copy, options);
	CHECK(!again.added);
	CHECK(again.duplicate);
	CHECK_EQ(again.id, first.id);
	CHECK_EQ(library.Count(kFilterAll), 2);
	// Not pictures.
	WriteFile(scratch + "/notes.txt", "x", 1);
	CHECK(!library.ImportFile(scratch + "/notes.txt", options).error.empty());
	CHECK(!library.ImportFile(scratch + "/clip.mov", options).error.empty());
	CHECK(!library.ImportFile(scratch + "/missing.jpg", options).error.empty());

	Photo photo;
	CHECK(library.Get(first.id, photo));
	CHECK_EQ(photo.name, std::string("IMG_0001.JPG"));
	CHECK_EQ(photo.width, 640);
	CHECK_EQ(photo.height, 480);
	CHECK_EQ(photo.taken, (int64_t)1700000000);
	CHECK(StartsWith(photo.path, "originals/2023/11/"));
	CHECK(FileExists(library.OriginalPath(photo)));
	CHECK_EQ(photo.sha1, Sha1OfFile(a));
	// The source is untouched by a copying import, taken by a moving one.
	CHECK(FileExists(a));
	std::string c = FakeJpeg(scratch, "IMG_0003.JPG", 100, 100, "three");
	options.move = true;
	options.itemKey = "img_0003.jpg|1|2";
	CHECK(library.ImportFile(c, options).added);
	CHECK(!FileExists(c));
	CHECK(library.WasImported("img_0003.jpg|1|2", "", 0));
	CHECK(library.WasImported("", "img_0001.jpg", photo.size));
	CHECK(!library.WasImported("other", "IMG_0001.JPG", photo.size + 1));

	std::vector<Import> imports;
	library.ListImports(imports);
	CHECK_EQ((int)imports.size(), 1);
	CHECK_EQ(imports[0].count, 3);
	CHECK_EQ(imports[0].source, std::string("Test"));

	// Two files of one name in one month get two names.
	std::string other = Scratch("import-other");
	std::string same = FakeJpeg(other, "IMG_0001.JPG", 640, 480, "another picture");
	options.move = false;
	options.itemKey.clear();
	ImportResult renamed = library.ImportFile(same, options);
	CHECK(renamed.added);
	Photo second1;
	CHECK(library.Get(renamed.id, second1));
	CHECK(second1.path != photo.path);
	CHECK_EQ(second1.name, std::string("IMG_0001.JPG"));
}


TEST(LibraryTrashAndVersions)
{
	std::string scratch = Scratch("trash");
	Library library;
	CHECK(library.Open(scratch + "/lib"));
	ImportOptions options;
	options.takenFallback = 1600000000;
	int64_t ids[3];
	for (int i = 0; i < 3; i++) {
		std::string name = "P" + std::to_string(i) + ".jpg";
		ids[i] = library.ImportFile(FakeJpeg(scratch, name, 30, 20, name), options).id;
		CHECK(ids[i] != 0);
	}
	library.SetFavorite(ids[0], true);
	CHECK_EQ(library.Count(kFilterFavorites), 1);

	// The trash.
	library.MoveToTrash({ids[1]});
	CHECK_EQ(library.Count(kFilterAll), 2);
	CHECK_EQ(library.Count(kFilterTrash), 1);
	std::vector<PhotoBrief> photos;
	library.List(kFilterAll, 0, photos);
	CHECK_EQ((int)photos.size(), 2);
	library.RestoreFromTrash({ids[1]});
	CHECK_EQ(library.Count(kFilterAll), 3);
	// Importing a picture that is in the trash brings it back.
	library.MoveToTrash({ids[2]});
	ImportResult back = library.ImportFile(FakeJpeg(scratch, "again.jpg", 30, 20, "P2.jpg"),
		options);
	CHECK(back.restored);
	CHECK_EQ(back.id, ids[2]);
	CHECK_EQ(library.Count(kFilterTrash), 0);

	// Versions.
	Photo photo;
	CHECK(library.Get(ids[0], photo));
	int64_t revision = photo.revision;
	CHECK_EQ(library.DisplayPath(photo), library.OriginalPath(photo));
	Version edit;
	edit.kind = kVersionEdit;
	edit.params = "{\"exposure\":0.5}";
	edit.width = 30;
	edit.height = 20;
	std::string rendered = FakeJpeg(scratch, "render.jpg", 30, 20, "rendered");
	int64_t first = library.AddVersion(ids[0], edit, rendered, false);
	CHECK(first != 0);
	CHECK(library.Get(ids[0], photo));
	CHECK_EQ(photo.version, first);
	CHECK(photo.revision > revision);
	CHECK(library.DisplayPath(photo) != library.OriginalPath(photo));
	CHECK(FileExists(library.DisplayPath(photo)));
	CHECK_EQ(library.Count(kFilterEdited), 1);

	Version external;
	external.kind = kVersionExternal;
	external.source = "PhotoCraft";
	external.base = first;
	int64_t second = library.AddVersion(ids[0], external,
		FakeJpeg(scratch, "saved.jpg", 30, 20, "saved elsewhere"), false);
	CHECK(second != 0);
	std::vector<Version> versions;
	library.ListVersions(ids[0], versions);
	CHECK_EQ((int)versions.size(), 2);
	CHECK(library.Get(ids[0], photo));
	CHECK_EQ(photo.version, second);
	// Back to the original and forth.
	CHECK(library.SetCurrentVersion(ids[0], 0));
	CHECK(library.Get(ids[0], photo));
	CHECK_EQ(photo.version, (int64_t)0);
	CHECK(library.SetCurrentVersion(ids[0], first));
	CHECK(!library.SetCurrentVersion(ids[1], first));	// another photo's
	// An edit version is replaced in place.
	std::string rerendered = FakeJpeg(scratch, "render2.jpg", 20, 30, "rendered again");
	CHECK(library.UpdateEditVersion(first, "{\"exposure\":1}", rerendered, 20, 30));
	Version read;
	CHECK(library.GetVersion(first, read));
	CHECK_EQ(read.width, 20);
	CHECK_EQ(read.params, std::string("{\"exposure\":1}"));
	// Deleting the version shown shows the latest that is left.
	CHECK(library.DeleteVersion(first));
	CHECK(library.Get(ids[0], photo));
	CHECK_EQ(photo.version, second);
	CHECK(library.GetVersion(second, read));
	CHECK_EQ(read.base, (int64_t)0);
	CHECK(library.DeleteVersion(second));
	CHECK(library.Get(ids[0], photo));
	CHECK_EQ(photo.version, (int64_t)0);

	// Deleting for good takes the files along.
	std::string original = library.OriginalPath(photo);
	library.DeletePermanently({ids[0]});
	CHECK(!FileExists(original));
	CHECK(!library.Get(ids[0], photo));
	CHECK_EQ(library.Count(kFilterAll), 2);
	library.MoveToTrash({ids[1], ids[2]});
	library.ExpireTrash(30);
	CHECK_EQ(library.Count(kFilterTrash), 2);
	library.EmptyTrash();
	CHECK_EQ(library.Count(kFilterTrash), 0);

	// Settings live with the library.
	library.SetSetting("key", "value");
	library.SetSetting("key", "other");
	CHECK_EQ(library.Setting("key"), std::string("other"));
	CHECK_EQ(library.Setting("none", "fallback"), std::string("fallback"));

	// Opening again finds everything.
	library.Close();
	Library reopened;
	CHECK(reopened.Open(scratch + "/lib"));
	CHECK_EQ(reopened.Count(kFilterAll), 0);
	CHECK_EQ(reopened.Setting("key"), std::string("other"));
}


namespace {

// A unit vector near one of a few fixed directions: "the same person".
std::vector<float>
Embedding(int person, float noise, std::mt19937& random)
{
	const int kDimension = 64;
	std::mt19937 base(person * 7919 + 13);
	std::normal_distribution<float> normal(0, 1);
	std::vector<float> vector(kDimension);
	double length = 0;
	for (int i = 0; i < kDimension; i++) {
		vector[i] = normal(base) + noise * normal(random);
		length += vector[i] * vector[i];
	}
	for (float& value : vector)
		value /= (float)sqrt(length);
	return vector;
}


struct PeopleFixture {
	std::string scratch;
	Library library;
	People people;
	std::mt19937 random;
	int counter = 0;

	explicit PeopleFixture(const char* name)
		:
		scratch(Scratch(name)),
		people(library),
		random(42)
	{
		library.Open(scratch + "/lib");
	}

	// A photo with one face of each of the persons given.
	std::vector<Face> Photo(const std::vector<int>& persons, float noise = 0.25f,
		float quality = 0.8f)
	{
		std::string name = "F" + std::to_string(counter++) + ".jpg";
		ImportOptions options;
		options.takenFallback = 1500000000 + counter * 86400;
		int64_t id = library.ImportFile(FakeJpeg(scratch, name, 40, 30, name), options).id;
		std::vector<Face> faces;
		float x = 0.05f;
		for (int person : persons) {
			Face face;
			face.x = x;
			face.y = 0.2f;
			face.width = 0.15f;
			face.height = 0.2f;
			x += 0.2f;
			face.score = 0.9f;
			face.quality = quality;
			face.embedding = Embedding(person, noise, random);
			faces.push_back(face);
		}
		library.SetFaces(id, faces, 1);
		people.Place(faces);
		return faces;
	}
};

}	// namespace


TEST(PeopleGroupNameAndRecognize)
{
	PeopleFixture f("people");
	// Three people over a number of photos, sometimes together.
	for (int i = 0; i < 6; i++)
		f.Photo({1});
	for (int i = 0; i < 4; i++)
		f.Photo({2});
	f.Photo({1, 2});
	f.Photo({3});
	f.Photo({3, 1});

	std::vector<FaceCluster> clusters;
	f.people.ListClusters(clusters);
	CHECK_EQ((int)clusters.size(), 3);
	CHECK_EQ((int)clusters[0].faces.size(), 8);		// person 1
	CHECK_EQ(clusters[0].photoCount, 8);
	CHECK_EQ((int)clusters[1].faces.size(), 5);
	CHECK_EQ((int)clusters[2].faces.size(), 2);
	std::vector<FaceCluster> larger;
	f.people.ListClusters(larger, 3);
	CHECK_EQ((int)larger.size(), 2);

	// Naming the largest group makes a person of all its faces.
	int64_t anna = f.people.NameCluster(clusters[0].id, "  Anna ");
	CHECK(anna != 0);
	Person person;
	CHECK(f.library.GetPerson(anna, person));
	CHECK_EQ(person.name, std::string("Anna"));
	CHECK_EQ(person.faceCount, 8);
	CHECK_EQ(person.photoCount, 8);
	CHECK(person.coverFace != 0);
	std::vector<PhotoBrief> photos;
	f.library.List(kFilterPerson, anna, photos);
	CHECK_EQ((int)photos.size(), 8);
	f.people.ListClusters(clusters);
	CHECK_EQ((int)clusters.size(), 2);

	// A new photo of her is recognised; one of a stranger is not.
	std::vector<Face> fresh = f.Photo({1, 4});
	Face face;
	CHECK(f.library.GetFace(fresh[0].id, face));
	CHECK_EQ(face.personId, anna);
	CHECK_EQ(face.source, (int)kFaceAuto);
	CHECK(f.library.GetFace(fresh[1].id, face));
	CHECK_EQ(face.personId, (int64_t)0);

	// The same name again is the same person.
	CHECK_EQ(f.people.NameCluster(clusters[0].id, "anna"), anna);
	CHECK(f.library.GetPerson(anna, person));
	CHECK_EQ(person.faceCount, 9 + 5);
	// Undo that with a rename and a second person: name the other group.
	f.people.ListClusters(clusters);
	int64_t carl = f.people.NameCluster(clusters[0].id, "Carl");
	CHECK(carl != 0 && carl != anna);

	// Survives a restart.
	People reloaded(f.library);
	reloaded.Load();
	CHECK_EQ(reloaded.CountFaces(), f.people.CountFaces());
	CHECK(reloaded.BestFace(anna) != 0);
	std::vector<FaceCluster> after;
	reloaded.ListClusters(after);
	f.people.ListClusters(clusters);
	CHECK_EQ(after.size(), clusters.size());
}


TEST(PeopleSuggestConfirmReject)
{
	PeopleFixture f("suggest");
	PeopleThresholds thresholds;
	f.people.SetThresholds(thresholds);
	for (int i = 0; i < 5; i++)
		f.Photo({1}, 0.2f);
	std::vector<FaceCluster> clusters;
	f.people.ListClusters(clusters);
	int64_t anna = f.people.NameCluster(clusters[0].id, "Anna");

	// A noisy face of hers: not sure enough to assign, good enough to ask.
	std::vector<Face> unsure = f.Photo({1}, 0.75f);
	Face face;
	CHECK(f.library.GetFace(unsure[0].id, face));
	std::vector<FaceSuggestion> suggestions;
	f.people.Suggestions(anna, suggestions);
	if (face.personId == 0) {
		bool listed = false;
		for (auto& suggestion : suggestions)
			listed = listed || suggestion.faceId == unsure[0].id;
		CHECK(listed);
		// "Yes": hers, by the user's word.
		f.people.ConfirmFace(unsure[0].id, anna);
		CHECK(f.library.GetFace(unsure[0].id, face));
		CHECK_EQ(face.personId, anna);
		CHECK_EQ(face.source, (int)kFaceUser);
	}

	// "No": never hers again, however alike.
	std::vector<Face> other = f.Photo({1}, 0.2f);
	CHECK(f.library.GetFace(other[0].id, face));
	CHECK_EQ(face.personId, anna);
	f.people.RejectFace(other[0].id, anna);
	CHECK(f.library.GetFace(other[0].id, face));
	CHECK_EQ(face.personId, (int64_t)0);
	CHECK(f.library.IsRejected(other[0].id, anna));
	CHECK_EQ(f.people.Recognize(anna), 0);
	f.people.Suggestions(anna, suggestions);
	for (auto& suggestion : suggestions)
		CHECK(suggestion.faceId != other[0].id);
	People reloaded(f.library);
	reloaded.Load();
	CHECK_EQ(reloaded.Recognize(0), 0);

	// A blurred face is kept out of all of it.
	std::vector<Face> blurred = f.Photo({1}, 0.2f, 0.1f);
	CHECK(f.library.GetFace(blurred[0].id, face));
	CHECK_EQ(face.personId, (int64_t)0);
	CHECK_EQ(face.cluster, (int64_t)0);

	// Forgetting the person gives the faces back to the unnamed ones.
	f.people.DeletePerson(anna);
	Person person;
	CHECK(!f.library.GetPerson(anna, person));
	f.people.ListClusters(clusters);
	CHECK(!clusters.empty());
	CHECK(clusters[0].faces.size() >= 5);
}


TEST(PeopleOncePerPhoto)
{
	PeopleFixture f("once");
	// A pattern mistaken for a face seven times in one photo: seven groups
	// of one, not a group of seven.
	f.Photo({9, 9, 9, 9, 9, 9, 9}, 0.05f);
	std::vector<FaceCluster> clusters;
	f.people.ListClusters(clusters);
	CHECK_EQ((int)clusters.size(), 7);
	f.people.ListClusters(clusters, 2);
	CHECK_EQ((int)clusters.size(), 0);

	// A person is assigned once in a photo even when two faces look like
	// them.
	for (int i = 0; i < 4; i++)
		f.Photo({1}, 0.2f);
	f.people.ListClusters(clusters, 2);
	CHECK_EQ((int)clusters.size(), 1);
	int64_t anna = f.people.NameCluster(clusters[0].id, "Anna");
	std::vector<Face> twins = f.Photo({1, 1}, 0.15f);
	Face first, second;
	CHECK(f.library.GetFace(twins[0].id, first));
	CHECK(f.library.GetFace(twins[1].id, second));
	CHECK((first.personId == anna) != (second.personId == anna));

	// Merging two people.
	for (int i = 0; i < 3; i++)
		f.Photo({2}, 0.2f);
	f.people.ListClusters(clusters, 2);
	int64_t bert = f.people.NameCluster(clusters[0].id, "Bert");
	CHECK(bert != 0 && bert != anna);
	f.people.MergePeople(bert, anna);
	Person person;
	CHECK(!f.library.GetPerson(bert, person));
	CHECK(f.library.GetPerson(anna, person));
	CHECK(person.photoCount >= 8);
}


TEST(FacesFollowTheirPhoto)
{
	PeopleFixture f("faces");
	std::vector<Face> faces = f.Photo({1, 2});
	int64_t photoId = faces[0].photoId;
	std::vector<FaceCluster> clusters;
	f.people.ListClusters(clusters);
	int64_t anna = f.people.NameFace(faces[0].id, "Anna");
	CHECK(anna != 0);

	// A second scan of the photo (a new model) keeps the name on the face
	// in the same place.
	std::vector<Face> rescanned;
	for (auto& old : faces) {
		Face face = old;
		face.id = 0;
		face.personId = 0;
		face.source = 0;
		face.x += 0.01f;
		rescanned.push_back(face);
	}
	f.people.Forget(photoId);
	f.library.SetFaces(photoId, rescanned, 2);
	f.people.Place(rescanned);
	std::vector<Face> now;
	f.library.ListFaces(photoId, now);
	CHECK_EQ((int)now.size(), 2);
	CHECK_EQ(now[0].personId, anna);
	CHECK_EQ(now[1].personId, (int64_t)0);
	Photo photo;
	CHECK(f.library.Get(photoId, photo));
	CHECK_EQ(photo.facesVersion, 2);

	float x, y, w, h;
	FaceCropRect(now[0], 400, 300, x, y, w, h);
	CHECK(x >= 0 && y >= 0 && x + w <= 1.0001f && y + h <= 1.0001f);
	CHECK(fabs(w * 400 - h * 300) < 1.0f);		// square in pixels

	// The photo's faces go with it.
	f.library.DeletePermanently({photoId});
	std::vector<Face> all;
	f.library.ListAllFaces(all);
	CHECK(all.empty());
}
