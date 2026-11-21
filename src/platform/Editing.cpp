#include "Editing.h"

#include "ImageIO.h"

namespace photos {

bool
GetEditTarget(Library& library, const Photo& photo, EditTarget& target)
{
	target = EditTarget();
	target.base = photo.version;
	target.baseWidth = photo.width;
	target.baseHeight = photo.height;
	Version current;
	if (photo.version != 0 && library.GetVersion(photo.version, current)) {
		if (current.kind == kVersionEdit) {
			target.base = current.base;
			target.replace = current.id;
			EditParams::FromJson(current.params, target.params);
		} else {
			target.baseWidth = current.width;
			target.baseHeight = current.height;
		}
	}
	if (target.replace != 0 && target.base != 0) {
		Version base;
		if (library.GetVersion(target.base, base)) {
			target.baseWidth = base.width;
			target.baseHeight = base.height;
		}
	}
	return true;
}


bool
SaveEdit(Library& library, Pictures& pictures, int64_t photoId,
	const EditParams& params, std::string& error)
{
	Photo photo;
	if (!library.Get(photoId, photo)) {
		error = "the photo is gone";
		return false;
	}
	EditTarget target;
	GetEditTarget(library, photo, target);
	if (params.IsIdentity()) {
		// Nothing left of the edit: show its base again.
		if (target.replace != 0) {
			library.SetCurrentVersion(photoId, target.base);
			library.DeleteVersion(target.replace);
		}
		return true;
	}
	Image source, result;
	if (pictures.LoadBase(photo, target.base, source, 0, &error) != B_OK) {
		if (error.empty())
			error = "the picture cannot be read";
		return false;
	}
	RenderEdit(source, params, result);
	source.Clear();
	std::string temporary = JoinPath(library.Directory(),
		"versions/.render-" + RandomId() + ".jpg");
	MakeDirs(DirName(temporary));
	if (!WriteJpeg(result, 93, temporary)) {
		error = "cannot write the edited picture";
		return false;
	}
	bool ok;
	if (target.replace != 0) {
		ok = library.UpdateEditVersion(target.replace, params.ToJson(), temporary,
			result.width, result.height);
	} else {
		Version version;
		version.kind = kVersionEdit;
		version.base = target.base;
		version.params = params.ToJson();
		version.width = result.width;
		version.height = result.height;
		ok = library.AddVersion(photoId, version, temporary, true) != 0;
	}
	RemoveFile(temporary);
	if (!ok)
		error = "cannot store the edited picture";
	return ok;
}


bool
RotatePhoto(Library& library, Pictures& pictures, int64_t photoId,
	int quarterTurns, std::string& error)
{
	Photo photo;
	if (!library.Get(photoId, photo)) {
		error = "the photo is gone";
		return false;
	}
	EditTarget target;
	GetEditTarget(library, photo, target);
	EditParams params = target.params;
	int turns = ((quarterTurns % 4) + 4) % 4;
	if (turns == 0)
		return true;
	// The crop frame turns with the picture.
	for (int i = 0; i < turns; i++) {
		float x = params.cropX, y = params.cropY, w = params.cropW, h = params.cropH;
		params.cropX = 1 - y - h;
		params.cropY = x;
		params.cropW = h;
		params.cropH = w;
	}
	// A mirrored picture turns the other way round underneath.
	params.rotation = (params.rotation + (params.flip ? 4 - turns : turns)) % 4;
	return SaveEdit(library, pictures, photoId, params, error);
}


bool
ExportPhoto(Library& library, Pictures& pictures, int64_t photoId,
	const std::string& directory, bool keepOriginal, std::string& written,
	std::string& error)
{
	Photo photo;
	if (!library.Get(photoId, photo)) {
		error = "the photo is gone";
		return false;
	}
	std::string source = library.DisplayPath(photo);
	if (source.empty() || (photo.version == 0 && keepOriginal)) {
		// The original itself; from the server if it is not here.
		if (photo.path.empty()) {
			Image ignored;
			if (pictures.Load(photo, ignored, 64, true, true, &error) != B_OK
				|| !library.Get(photoId, photo)) {
				if (error.empty())
					error = "the original is not available";
				return false;
			}
			source = library.DisplayPath(photo);
		}
	}
	std::string name = photo.name;
	MediaKind kind = KindOfName(source);
	bool copy = (photo.version == 0 && keepOriginal) || kind == kKindJpeg;
	if (copy) {
		if (photo.version != 0)
			name = StripExtension(photo.name) + "." + Extension(source);
		name = UniqueName(directory, name);
		written = JoinPath(directory, name);
		if (!CopyFile(source, written)) {
			error = "cannot write " + written;
			return false;
		}
		return true;
	}
	Image image;
	if (pictures.Load(photo, image, 0, false, true, &error) != B_OK)
		return false;
	name = UniqueName(directory, StripExtension(photo.name) + ".jpg");
	written = JoinPath(directory, name);
	if (!WriteJpeg(image, 93, written)) {
		error = "cannot write " + written;
		return false;
	}
	return true;
}

}	// namespace photos
