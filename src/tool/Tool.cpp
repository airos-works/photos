// photos_tool: the Photos library from a shell. For tests and scripting;
// it works on the same library and settings as the application
// (PHOTOS_LIBRARY and PHOTOS_SETTINGS_DIR choose others).
#include "core/Edit.h"
#include "platform/ImageIO.h"
#include "platform/Session.h"

#include <Application.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace photos;

static int
Usage()
{
	fprintf(stderr,
		"photos_tool COMMAND ...\n"
		"  info                         the library and what is in it\n"
		"  import PATH...               files and folders\n"
		"  list [COUNT]                 the newest photos\n"
		"  show ID                      one photo's details and versions\n"
		"  thumbs                       makes every missing thumbnail\n"
		"  decode FILE OUT.jpg [EDGE] [preview]   decodes any picture file\n"
		"  edit ID JSON                 applies an edit (EditParams as JSON)\n"
		"  revert ID                    back to the original\n"
		"  trash ID... | restore ID... | delete ID... | empty-trash\n"
		"  favorite ID 0|1\n"
		"  signin SERVER EMAIL PASSWORD | apikey SERVER KEY | signout\n"
		"  sync [--no-upload]           one pass with the Immich server\n"
		"  fetch ID                     downloads a photo's original from Immich\n"
		"  faces [--cpu]                scans every unscanned photo\n"
		"  people                       people and groups of unnamed faces\n"
		"  name CLUSTER NAME            names a group\n"
		"  suggestions PERSON           faces that may be that person\n"
		"  confirm FACE PERSON | reject FACE PERSON\n"
		"  working-file ID              the file an editor would get\n"
		"  check-edits ID               takes saves in the working folder as versions\n"
		"  set KEY VALUE | get KEY      settings\n");
	return 2;
}


static std::string
When(int64_t seconds)
{
	CivilTime time = SecondsToCivil(seconds);
	char text[64];
	snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", time.year,
		time.month, time.day, time.hour, time.minute, time.second);
	return text;
}


static void
PrintPhoto(Session& session, int64_t id, bool details)
{
	Photo photo;
	if (!session.Lib().Get(id, photo)) {
		printf("%lld: no such photo\n", (long long)id);
		return;
	}
	printf("%lld  %s  %s  %dx%d  %s%s%s%s%s\n", (long long)photo.id,
		When(photo.taken).c_str(), photo.name.c_str(), photo.width, photo.height,
		KindName(photo.kind), photo.path.empty() ? "  [on Immich only]" : "",
		photo.favorite ? "  [favorite]" : "", photo.trashed != 0 ? "  [trash]" : "",
		photo.version != 0 ? "  [edited]" : "");
	if (!details)
		return;
	printf("  file      %s (%s)\n", photo.path.c_str(), FormatSize(photo.size).c_str());
	printf("  sha1      %s\n", photo.sha1.c_str());
	printf("  camera    %s %s  %s\n", photo.make.c_str(), photo.model.c_str(),
		photo.lens.c_str());
	printf("  exposure  %s %s ISO %.0f %.0f mm\n",
		FormatExposure(photo.exposureTime).c_str(),
		FormatAperture(photo.fNumber).c_str(), photo.iso, photo.focalLength);
	if (photo.hasGps)
		printf("  place     %.5f, %.5f\n", photo.latitude, photo.longitude);
	printf("  immich    %s (state %d)\n", photo.immichId.c_str(), photo.immichState);
	printf("  revision  %lld, version shown %lld, faces scanned with model %d\n",
		(long long)photo.revision, (long long)photo.version, photo.facesVersion);
	std::vector<Version> versions;
	session.Lib().ListVersions(id, versions);
	for (auto& version : versions) {
		printf("  version %lld  %s  %s  %dx%d  %s%s%s\n", (long long)version.id,
			version.kind == kVersionEdit ? "edit" : "external",
			version.path.c_str(), version.width, version.height,
			version.kind == kVersionEdit ? version.params.c_str()
				: version.source.c_str(),
			version.document.empty() ? "" : "  document ",
			version.document.c_str());
	}
	std::vector<Face> faces;
	session.Lib().ListFaces(id, faces);
	for (auto& face : faces) {
		Person person;
		std::string who = "-";
		if (face.personId != 0 && session.Lib().GetPerson(face.personId, person))
			who = person.name;
		printf("  face %lld  %.3f,%.3f %.3fx%.3f  score %.2f quality %.2f  %s\n",
			(long long)face.id, face.x, face.y, face.width, face.height,
			face.score, face.quality, who.c_str());
	}
}


// Renders an edit of the photo's base picture into a version, as the editor
// does on "Done".
static bool
ApplyEdit(Session& session, int64_t id, const EditParams& params, std::string& error)
{
	Photo photo;
	if (!session.Lib().Get(id, photo)) {
		error = "no such photo";
		return false;
	}
	// An edit of an edit starts from what that edit started from.
	int64_t base = photo.version;
	int64_t replace = 0;
	Version current;
	if (photo.version != 0 && session.Lib().GetVersion(photo.version, current)
		&& current.kind == kVersionEdit) {
		base = current.base;
		replace = current.id;
	}
	Image source, result;
	if (session.Pics().LoadBase(photo, base, source, 0, &error) != B_OK)
		return false;
	RenderEdit(source, params, result);
	std::string temporary = JoinPath(session.Lib().Directory(),
		"versions/.render-" + RandomId() + ".jpg");
	MakeDirs(DirName(temporary));
	if (!WriteJpeg(result, 93, temporary)) {
		error = "cannot write the picture";
		return false;
	}
	bool ok;
	if (replace != 0) {
		ok = session.Lib().UpdateEditVersion(replace, params.ToJson(), temporary,
			result.width, result.height);
	} else {
		Version version;
		version.kind = kVersionEdit;
		version.base = base;
		version.params = params.ToJson();
		version.width = result.width;
		version.height = result.height;
		ok = session.Lib().AddVersion(id, version, temporary, true) != 0;
	}
	RemoveFile(temporary);
	if (!ok)
		error = "cannot store the version";
	return ok;
}


int
main(int argc, char** argv)
{
	if (argc < 2)
		return Usage();
	std::string command = argv[1];
	BApplication app("application/x-vnd.airos-Photos-Tool");

	if (command == "decode") {
		if (argc < 4)
			return Usage();
		Image image;
		std::string error;
		int edge = argc > 4 ? atoi(argv[4]) : 0;
		uint32 flags = kDecodeExact;
		if (argc > 5 && strcmp(argv[5], "preview") == 0)
			flags |= kDecodeRawPreview;
		bigtime_t start = system_time();
		status_t status = DecodeFile(argv[2], image, edge, flags, &error);
		if (status != B_OK) {
			fprintf(stderr, "%s: %s (%s)\n", argv[2], error.c_str(), strerror(status));
			return 1;
		}
		printf("%dx%d in %.0f ms\n", image.width, image.height,
			(system_time() - start) / 1000.0);
		return WriteJpeg(image, 90, argv[3]) ? 0 : 1;
	}

	Session session;
	std::string error;
	if (!session.Open(&error)) {
		fprintf(stderr, "cannot open the library: %s\n", error.c_str());
		return 1;
	}
	Library& library = session.Lib();

	if (command == "info") {
		printf("library   %s\n", library.Directory().c_str());
		printf("settings  %s\n", session.SettingsDirectory().c_str());
		printf("photos    %d (%d favorites, %d in the trash)\n",
			library.Count(kFilterAll), library.Count(kFilterFavorites),
			library.Count(kFilterTrash));
		std::vector<PhotoBrief> remote;
		library.ListRemoteOnly(remote);
		printf("on Immich only: %d; not on Immich: %d\n", (int)remote.size(),
			library.Count(kFilterLocalOnly));
		std::vector<Person> people;
		library.ListPeople(people, true);
		printf("people    %d, faces %d\n", (int)people.size(),
			session.Persons().CountFaces());
		printf("faces to scan: %d (model %d)\n",
			library.Count(kFilterNeedsFaces, FaceScanner::ModelVersion()),
			FaceScanner::ModelVersion());
		printf("raw engine: %s\n", HaveRawEngine() ? RawEngineVersion().c_str() : "none");
		printf("models    %s\n", session.ModelDirectory().c_str());
		printf("immich    %s %s\n",
			session.Prefs().GetString(setting::kImmichServer).c_str(),
			session.Sync().IsConfigured() ? "(signed in)" : "(not set up)");
		return 0;
	}

	if (command == "import") {
		std::vector<std::string> paths;
		for (int i = 2; i < argc; i++)
			paths.push_back(argv[i]);
		session.Imports().ImportPaths(paths, "Files");
		session.Imports().WaitUntilIdle();
		ImportStatus status = session.Imports().Status();
		printf("%d files: %d added, %d already there, %d failed\n", status.done,
			status.added, status.duplicates, status.failed);
		if (!status.error.empty())
			printf("first failure: %s\n", status.error.c_str());
		return status.failed == 0 ? 0 : 1;
	}

	if (command == "list") {
		std::vector<PhotoBrief> photos;
		library.List(kFilterAll, 0, photos);
		size_t count = argc > 2 ? (size_t)atoi(argv[2]) : 20;
		for (size_t i = 0; i < photos.size() && i < count; i++)
			PrintPhoto(session, photos[i].id, false);
		printf("%d photos\n", (int)photos.size());
		return 0;
	}

	if (command == "show" && argc > 2) {
		PrintPhoto(session, atoll(argv[2]), true);
		return 0;
	}

	if (command == "thumbs") {
		std::vector<PhotoBrief> photos;
		library.List(kFilterAll, 0, photos);
		int made = 0, failed = 0;
		bigtime_t start = system_time();
		for (auto& brief : photos) {
			Photo photo;
			if (!library.Get(brief.id, photo))
				continue;
			std::string message;
			if (session.Pics().EnsureThumb(photo, &message).empty()) {
				failed++;
				fprintf(stderr, "%s: %s\n", photo.name.c_str(), message.c_str());
			} else
				made++;
		}
		printf("%d thumbnails, %d failed, %.1f s\n", made, failed,
			(system_time() - start) / 1e6);
		return failed == 0 ? 0 : 1;
	}

	if (command == "edit" && argc > 3) {
		EditParams params;
		if (!EditParams::FromJson(argv[3], params)) {
			fprintf(stderr, "not an edit: %s\n", argv[3]);
			return 2;
		}
		if (!ApplyEdit(session, atoll(argv[2]), params, error)) {
			fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		PrintPhoto(session, atoll(argv[2]), true);
		return 0;
	}

	if (command == "revert" && argc > 2) {
		library.SetCurrentVersion(atoll(argv[2]), 0);
		PrintPhoto(session, atoll(argv[2]), true);
		return 0;
	}

	if ((command == "trash" || command == "restore" || command == "delete")
		&& argc > 2) {
		std::vector<int64_t> ids;
		for (int i = 2; i < argc; i++)
			ids.push_back(atoll(argv[i]));
		if (command == "trash")
			library.MoveToTrash(ids);
		else if (command == "restore")
			library.RestoreFromTrash(ids);
		else
			library.DeletePermanently(ids);
		return 0;
	}

	if (command == "empty-trash") {
		library.EmptyTrash();
		return 0;
	}

	if (command == "favorite" && argc > 3) {
		library.SetFavorite(atoll(argv[2]), atoi(argv[3]) != 0);
		return 0;
	}

	if (command == "signin" && argc > 4) {
		if (!session.SignIn(argv[2], argv[3], argv[4], error)) {
			fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		printf("signed in to %s\n",
			session.Prefs().GetString(setting::kImmichServer).c_str());
		return 0;
	}

	if (command == "apikey" && argc > 3) {
		if (!session.UseApiKey(argv[2], argv[3], error)) {
			fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		printf("using the key for %s\n",
			session.Prefs().GetString(setting::kImmichUser).c_str());
		return 0;
	}

	if (command == "signout") {
		session.SignOut();
		return 0;
	}

	if (command == "sync") {
		if (argc > 2 && strcmp(argv[2], "--no-upload") == 0) {
			SyncOptions options = session.Sync().Options();
			options.upload = false;
			session.Sync().SetOptions(options);
		}
		bigtime_t start = system_time();
		SyncResult result = session.SyncBlocking();
		printf("%s in %.1f s: %d new from the server, %d gone there, %d uploaded, "
			"%d matched, %d originals fetched, %d failed\n",
			result.ok ? "synced" : "sync FAILED", (system_time() - start) / 1e6,
			result.added, result.removed, result.uploaded, result.linked,
			result.downloaded, result.failed);
		if (!result.error.empty())
			printf("error: %s\n", result.error.c_str());
		if (!result.firstFailure.empty())
			printf("first failure: %s\n", result.firstFailure.c_str());
		return result.ok ? 0 : 1;
	}

	if (command == "fetch" && argc > 2) {
		if (!session.Sync().FetchOriginal(atoll(argv[2]), error)) {
			fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		PrintPhoto(session, atoll(argv[2]), true);
		return 0;
	}

	if (command == "faces") {
		bool cpu = argc > 2 && strcmp(argv[2], "--cpu") == 0;
		bigtime_t start = system_time();
		session.Faces().Configure(session.ModelDirectory(), cpu);
		if (!session.Faces().ScanAllNow(error)) {
			fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		FaceScanStatus status = session.Faces().Status();
		printf("%d photos scanned on %s in %.1f s; %d faces known\n", status.done,
			status.device.c_str(), (system_time() - start) / 1e6,
			session.Persons().CountFaces());
		return 0;
	}

	if (command == "people") {
		std::vector<Person> people;
		library.ListPeople(people, true);
		for (auto& person : people) {
			printf("person %lld  %-24s %d faces in %d photos%s\n",
				(long long)person.id, person.name.c_str(), person.faceCount,
				person.photoCount, person.hidden ? "  [hidden]" : "");
		}
		std::vector<FaceCluster> clusters;
		session.Persons().ListClusters(clusters);
		for (auto& cluster : clusters) {
			printf("group %lld  %d faces in %d photos (cover face %lld)\n",
				(long long)cluster.id, (int)cluster.faces.size(), cluster.photoCount,
				(long long)cluster.cover);
		}
		return 0;
	}

	if (command == "name" && argc > 3) {
		int64_t person = session.Persons().NameCluster(atoll(argv[2]), argv[3]);
		printf("person %lld\n", (long long)person);
		return person != 0 ? 0 : 1;
	}

	if (command == "suggestions" && argc > 2) {
		std::vector<FaceSuggestion> suggestions;
		session.Persons().Suggestions(atoll(argv[2]), suggestions);
		for (auto& suggestion : suggestions) {
			printf("face %lld in photo %lld  similarity %.3f\n",
				(long long)suggestion.faceId, (long long)suggestion.photoId,
				suggestion.similarity);
		}
		return 0;
	}

	if ((command == "confirm" || command == "reject") && argc > 3) {
		if (command == "confirm")
			session.Persons().ConfirmFace(atoll(argv[2]), atoll(argv[3]));
		else
			session.Persons().RejectFace(atoll(argv[2]), atoll(argv[3]));
		return 0;
	}

	if (command == "working-file" && argc > 2) {
		std::string path;
		session.Editors().Run();
		status_t status = session.Editors().PrepareWorkingFile(atoll(argv[2]), path,
			&error);
		if (status != B_OK) {
			fprintf(stderr, "%s (%s)\n", error.c_str(), strerror(status));
			return 1;
		}
		printf("%s\n", path.c_str());
		return 0;
	}

	if (command == "check-edits" && argc > 2) {
		session.Editors().Run();
		int added = session.Editors().CheckNow(atoll(argv[2]));
		printf("%d new versions\n", added);
		PrintPhoto(session, atoll(argv[2]), true);
		return 0;
	}

	if (command == "set" && argc > 3) {
		std::string value = argv[3];
		if (value == "true" || value == "false")
			session.Prefs().SetBool(argv[2], value == "true");
		else if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos)
			session.Prefs().SetInt(argv[2], atoll(value.c_str()));
		else
			session.Prefs().SetString(argv[2], value);
		session.Prefs().Save();
		return 0;
	}

	if (command == "get" && argc > 2) {
		printf("%s\n", session.Prefs().GetString(argv[2], "(not a string)").c_str());
		return 0;
	}

	return Usage();
}
