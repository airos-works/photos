#include "PhotoCraft.h"

#include "ImageIO.h"
#include "Messages.h"

#include <Directory.h>
#include <Entry.h>
#include <Message.h>
#include <MessageRunner.h>
#include <NodeMonitor.h>
#include <Path.h>
#include <Roster.h>

#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstring>

#include <sys/stat.h>

namespace photos {

const char* const kPhotoCraftSignature = "application/x-vnd.storytold-PhotoCraft";
const char* const kLightCraftSignature = "application/x-vnd.jmgasper-LightCraft";

namespace {

const uint32 kMsgCheckFolders = 'pcCk';
const bigtime_t kSettleTime = 800000;
const char* const kStateFile = ".photos-state";


bool
IsFlatPicture(const std::string& name)
{
	std::string ext = Extension(name);
	return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "tif"
		|| ext == "tiff" || ext == "webp" || ext == "bmp";
}


bool
IsDocument(const std::string& name)
{
	std::string ext = Extension(name);
	return ext == "psd" || ext == "pcraft";
}


uint32_t
Le32(const std::string& data, size_t at)
{
	if (at + 4 > data.size())
		return 0;
	const uint8_t* p = (const uint8_t*)data.data() + at;
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}


uint16_t
Le16(const std::string& data, size_t at)
{
	if (at + 2 > data.size())
		return 0;
	const uint8_t* p = (const uint8_t*)data.data() + at;
	return (uint16_t)(p[0] | p[1] << 8);
}

}	// namespace


bool
ExtractPcraftComposite(const std::string& path, std::string& png, bool& fullSize)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	bool found = false;
	// The end-of-central-directory record is in the last 64 KiB.
	fseeko(file, 0, SEEK_END);
	off_t size = ftello(file);
	size_t tailSize = (size_t)std::min<off_t>(size, 65536 + 22);
	std::string tail(tailSize, '\0');
	fseeko(file, size - (off_t)tailSize, SEEK_SET);
	if (fread(&tail[0], 1, tailSize, file) == tailSize) {
		size_t end = std::string::npos;
		for (size_t i = tailSize >= 22 ? tailSize - 22 : 0; i != (size_t)-1; i--) {
			if (Le32(tail, i) == 0x06054b50) {
				end = i;
				break;
			}
			if (i == 0)
				break;
		}
		if (end != std::string::npos) {
			uint32_t count = Le16(tail, end + 10);
			uint32_t directorySize = Le32(tail, end + 12);
			uint32_t directoryOffset = Le32(tail, end + 16);
			std::string directory(directorySize, '\0');
			if ((off_t)directoryOffset + directorySize <= size
				&& directorySize < (64u << 20)
				&& fseeko(file, directoryOffset, SEEK_SET) == 0
				&& fread(&directory[0], 1, directorySize, file) == directorySize) {
				// Prefer composite/full.png over composite/preview.png.
				uint32_t bestOffset = 0, bestSize = 0;
				int best = 0;
				size_t at = 0;
				for (uint32_t i = 0; i < count && at + 46 <= directory.size(); i++) {
					if (Le32(directory, at) != 0x02014b50)
						break;
					uint16_t method = Le16(directory, at + 10);
					uint32_t compressed = Le32(directory, at + 20);
					uint32_t stored = Le32(directory, at + 24);
					uint16_t nameLength = Le16(directory, at + 28);
					uint16_t extraLength = Le16(directory, at + 30);
					uint16_t commentLength = Le16(directory, at + 32);
					uint32_t localOffset = Le32(directory, at + 42);
					if (at + 46 + nameLength > directory.size())
						break;
					std::string name = directory.substr(at + 46, nameLength);
					int rank = name == "composite/full.png" ? 2
						: name == "composite/preview.png" ? 1 : 0;
					if (rank > best && method == 0 && compressed == stored
						&& stored > 0 && stored < (512u << 20)) {
						best = rank;
						bestOffset = localOffset;
						bestSize = stored;
					}
					at += 46 + nameLength + extraLength + commentLength;
				}
				if (best > 0) {
					char local[30];
					if (fseeko(file, bestOffset, SEEK_SET) == 0
						&& fread(local, 1, 30, file) == 30) {
						std::string header(local, 30);
						if (Le32(header, 0) == 0x04034b50) {
							off_t data = (off_t)bestOffset + 30 + Le16(header, 26)
								+ Le16(header, 28);
							png.resize(bestSize);
							if (data + (off_t)bestSize <= size
								&& fseeko(file, data, SEEK_SET) == 0
								&& fread(&png[0], 1, bestSize, file) == bestSize) {
								found = true;
								fullSize = best == 2;
							}
						}
					}
				}
			}
		}
	}
	fclose(file);
	return found;
}


PhotoCraftBridge::PhotoCraftBridge(Library& library, Pictures& pictures,
	SyncEngine& sync)
	:
	BLooper("PhotoCraft bridge"),
	fLibrary(library),
	fPictures(pictures),
	fSync(sync),
	fRunner(nullptr)
{
}


PhotoCraftBridge::~PhotoCraftBridge()
{
	stop_watching(this);
	delete fRunner;
}


bool
PhotoCraftBridge::IsInstalled(const char* signature)
{
	entry_ref ref;
	return be_roster->FindApp(signature, &ref) == B_OK;
}


void
PhotoCraftBridge::SetTarget(const BMessenger& target)
{
	if (Lock()) {
		fTarget = target;
		Unlock();
	}
}


void
PhotoCraftBridge::Start()
{
	Run();
	std::vector<std::string> names;
	ListDirectory(JoinPath(fLibrary.Directory(), "edits"), names);
	if (!Lock())
		return;
	for (auto& name : names) {
		int64_t id = atoll(name.c_str());
		if (id > 0)
			_Watch(id);
	}
	Unlock();
	// What was saved while Photos was away.
	PostMessage(kMsgCheckFolders);
}


void
PhotoCraftBridge::_LoadState(int64_t photoId, Folder& folder)
{
	std::string text;
	if (!ReadFile(JoinPath(fLibrary.EditsDirectory(photoId), kStateFile), text, 1 << 20))
		return;
	nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
	if (!parsed.is_object())
		return;
	for (auto it = parsed.begin(); it != parsed.end(); ++it) {
		if (!it.value().is_object())
			continue;
		FileState state;
		state.size = it.value().value("size", (int64_t)0);
		state.modified = it.value().value("modified", (int64_t)0);
		state.sha1 = it.value().value("sha1", std::string());
		folder.files[it.key()] = state;
	}
}


void
PhotoCraftBridge::_SaveState(int64_t photoId, const Folder& folder)
{
	nlohmann::json json = nlohmann::json::object();
	for (auto& file : folder.files) {
		json[file.first] = {{"size", file.second.size},
			{"modified", file.second.modified}, {"sha1", file.second.sha1}};
	}
	std::string text = json.dump();
	WriteFile(JoinPath(fLibrary.EditsDirectory(photoId), kStateFile), text.data(),
		text.size());
}


void
PhotoCraftBridge::_Watch(int64_t photoId)
{
	if (fFolders.count(photoId) != 0)
		return;
	std::string path = fLibrary.EditsDirectory(photoId);
	BEntry entry(path.c_str());
	Folder folder;
	if (entry.GetNodeRef(&folder.directory) != B_OK)
		return;
	_LoadState(photoId, folder);
	watch_node(&folder.directory, B_WATCH_DIRECTORY, this);
	fFolders[photoId] = folder;
	// Files written in place change no directory entry: watch each.
	std::vector<std::string> names;
	ListDirectory(path, names);
	for (auto& name : names) {
		node_ref ref;
		BEntry file(JoinPath(path, name).c_str());
		if (file.GetNodeRef(&ref) == B_OK)
			watch_node(&ref, B_WATCH_STAT, this);
	}
}


void
PhotoCraftBridge::_Schedule()
{
	delete fRunner;
	BMessage message(kMsgCheckFolders);
	fRunner = new BMessageRunner(BMessenger(this), &message, kSettleTime, 1);
}


void
PhotoCraftBridge::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_NODE_MONITOR:
			// Something happened in a working folder: look once things
			// have been quiet for a moment.
			_Schedule();
			break;

		case kMsgCheckFolders:
		{
			bool again = false;
			std::vector<int64_t> ids;
			for (auto& folder : fFolders)
				ids.push_back(folder.first);
			for (int64_t id : ids) {
				bool unstable = false;
				_Check(id, &unstable);
				again = again || unstable;
			}
			if (again)
				_Schedule();
			break;
		}

		default:
			BLooper::MessageReceived(message);
	}
}


int
PhotoCraftBridge::CheckNow(int64_t photoId)
{
	if (!Lock())
		return 0;
	_Watch(photoId);
	int added = 0;
	// Twice: a file is taken when it was the same at two looks.
	for (int i = 0; i < 2; i++) {
		bool again = false;
		added += _Check(photoId, &again);
	}
	Unlock();
	return added;
}


int
PhotoCraftBridge::_Check(int64_t photoId, bool* again)
{
	auto found = fFolders.find(photoId);
	if (found == fFolders.end())
		return 0;
	Folder& folder = found->second;
	std::string path = fLibrary.EditsDirectory(photoId);
	std::vector<std::string> names;
	ListDirectory(path, names);
	int added = 0;
	bool changed = false;
	for (auto& name : names) {
		if (name.empty() || name[0] == '.' || EndsWith(name, ".part")
			|| EndsWith(name, ".tmp") || EndsWith(name, "~"))
			continue;
		if (!IsFlatPicture(name) && !IsDocument(name))
			continue;
		std::string file = JoinPath(path, name);
		node_ref ref;
		BEntry entry(file.c_str());
		if (entry.GetNodeRef(&ref) == B_OK)
			watch_node(&ref, B_WATCH_STAT, this);
		FileState now;
		now.size = FileSize(file);
		now.modified = FileModified(file);
		if (now.size <= 0)
			continue;
		auto known = folder.files.find(name);
		if (known != folder.files.end() && known->second.size == now.size
			&& known->second.modified == now.modified) {
			folder.unstable.erase(name);
			continue;
		}
		// Changed: take it once it has stopped changing.
		auto pending = folder.unstable.find(name);
		if (pending == folder.unstable.end() || pending->second.size != now.size
			|| pending->second.modified != now.modified) {
			folder.unstable[name] = now;
			if (again != nullptr)
				*again = true;
			continue;
		}
		folder.unstable.erase(name);
		now.sha1 = Sha1OfFile(file);
		bool isNew = known == folder.files.end() || known->second.sha1 != now.sha1;
		folder.files[name] = now;
		changed = true;
		if (!isNew)
			continue;
		std::string note;
		if (_AddVersion(photoId, file, note))
			added++;
		else if (note.empty())
			continue;
		BMessage message(kMsgVersionAdded);
		message.AddInt64("id", photoId);
		if (!note.empty())
			message.AddString("note", note.c_str());
		fTarget.SendMessage(&message);
	}
	if (changed)
		_SaveState(photoId, folder);
	return added;
}


bool
PhotoCraftBridge::_AddVersion(int64_t photoId, const std::string& file,
	std::string& note)
{
	Photo photo;
	if (!fLibrary.Get(photoId, photo))
		return false;
	std::string name = BaseName(file);
	Version version;
	version.kind = kVersionExternal;
	version.source = "PhotoCraft";
	version.base = photo.version;
	std::string relativeEdits = "edits/" + std::to_string(photoId) + "/";

	if (IsFlatPicture(name)) {
		Image image;
		if (DecodeFile(file, image, 0, 0) != B_OK || image.IsEmpty()) {
			note = name + " was saved but cannot be read as a picture.";
			return false;
		}
		version.width = image.width;
		version.height = image.height;
		return fLibrary.AddVersion(photoId, version, file, false) != 0;
	}

	// A layered document: its composite is the picture.
	Image image;
	bool lowResolution = false;
	if (Extension(name) == "pcraft") {
		std::string png;
		bool fullSize = false;
		if (!ExtractPcraftComposite(file, png, fullSize)
			|| DecodeMemory(png.data(), png.size(), image) != B_OK) {
			note = name + " has no composite picture Photos could read.";
			return false;
		}
		lowResolution = !fullSize && std::max(image.width, image.height) >= 1024;
	} else if (DecodeFile(file, image, 0, 0) != B_OK || image.IsEmpty()) {
		note = name + " was saved but its merged picture cannot be read "
			"(save it with \"Maximize Compatibility\").";
		return false;
	}
	std::string rendered = JoinPath(fLibrary.EditsDirectory(photoId),
		".composite-" + RandomId() + ".jpg");
	if (!WriteJpeg(image, 95, rendered))
		return false;
	version.width = image.width;
	version.height = image.height;
	version.document = relativeEdits + name;
	bool ok = fLibrary.AddVersion(photoId, version, rendered, true) != 0;
	RemoveFile(rendered);
	if (ok && lowResolution) {
		note = "The .pcraft document holds only a small preview of the picture. "
			"For a full-size version turn on \"Save Full-Size Composite in "
			".pcraft Files\" in PhotoCraft's File Handling preferences, or save "
			"as PSD, JPEG or PNG.";
	}
	return ok;
}


status_t
PhotoCraftBridge::PrepareWorkingFile(int64_t photoId, std::string& path,
	std::string* error)
{
	Photo photo;
	if (!fLibrary.Get(photoId, photo)) {
		if (error != nullptr)
			*error = "the photo is gone";
		return B_ENTRY_NOT_FOUND;
	}
	std::string directory = fLibrary.EditsDirectory(photoId);
	if (!MakeDirs(directory))
		return B_IO_ERROR;

	// The layered document of the version shown, if it is still there: the
	// work goes on where it stopped.
	if (photo.version != 0) {
		Version version;
		if (fLibrary.GetVersion(photo.version, version) && !version.document.empty()) {
			std::string document = fLibrary.AbsolutePath(version.document);
			if (FileExists(document)) {
				path = document;
				return B_OK;
			}
		}
	}

	if (photo.path.empty() && photo.version == 0) {
		std::string message;
		if (!fSync.FetchOriginal(photoId, message) || !fLibrary.Get(photoId, photo)) {
			if (error != nullptr)
				*error = "cannot get the original from Immich: " + message;
			return B_IO_ERROR;
		}
	}
	std::string source = fLibrary.DisplayPath(photo);
	if (source.empty()) {
		if (error != nullptr)
			*error = "the original is missing";
		return B_ENTRY_NOT_FOUND;
	}
	std::string stem = StripExtension(photo.name);
	MediaKind kind = KindOfName(source);
	std::string target;
	if (kind == kKindJpeg || kind == kKindPng) {
		target = JoinPath(directory, stem + "." + Extension(source));
		// The same picture is not copied again (the working file may hold
		// unsaved-elsewhere work only if it differs, and then it is a
		// version already).
		if (!FileExists(target) || Sha1OfFile(target) != Sha1OfFile(source)) {
			if (!CopyFile(source, target)) {
				if (error != nullptr)
					*error = "cannot write the working file";
				return B_IO_ERROR;
			}
		}
	} else {
		Image image;
		status_t status = fPictures.Load(photo, image, 0, false, true, error);
		if (status != B_OK)
			return status;
		target = JoinPath(directory, stem + ".png");
		if (!WritePng(image, target)) {
			if (error != nullptr)
				*error = "cannot write the working file";
			return B_IO_ERROR;
		}
	}
	// What Photos put there is not a new version when it is seen.
	if (Lock()) {
		_Watch(photoId);
		auto found = fFolders.find(photoId);
		if (found != fFolders.end()) {
			FileState state;
			state.size = FileSize(target);
			state.modified = FileModified(target);
			state.sha1 = Sha1OfFile(target);
			found->second.files[BaseName(target)] = state;
			found->second.unstable.erase(BaseName(target));
			_SaveState(photoId, found->second);
			node_ref ref;
			BEntry entry(target.c_str());
			if (entry.GetNodeRef(&ref) == B_OK)
				watch_node(&ref, B_WATCH_STAT, this);
		}
		Unlock();
	}
	path = target;
	return B_OK;
}


status_t
PhotoCraftBridge::OpenInEditor(int64_t photoId, const char* signature,
	std::string* error)
{
	if (!IsInstalled(signature)) {
		if (error != nullptr) {
			*error = strcmp(signature, kLightCraftSignature) == 0
				? "LightCraft is not installed." : "PhotoCraft is not installed.";
		}
		return B_ENTRY_NOT_FOUND;
	}
	std::string path;
	status_t status = PrepareWorkingFile(photoId, path, error);
	if (status != B_OK)
		return status;
	entry_ref ref;
	status = get_ref_for_path(path.c_str(), &ref);
	if (status != B_OK)
		return status;
	BMessage refs(B_REFS_RECEIVED);
	refs.AddRef("refs", &ref);
	status = be_roster->Launch(signature, &refs);
	if (status == B_ALREADY_RUNNING)
		status = B_OK;
	if (status == B_OK) {
		team_id team = be_roster->TeamFor(signature);
		if (team >= 0)
			be_roster->ActivateApp(team);
	} else if (error != nullptr)
		*error = std::string("cannot start the editor: ") + strerror(status);
	return status;
}

}	// namespace photos
