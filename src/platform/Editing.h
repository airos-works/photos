// Applying edits to photos of the library: where an edit starts from, and
// storing its rendered picture as a version.
#pragma once

#include "core/Edit.h"
#include "core/Library.h"
#include "platform/Pictures.h"

#include <string>

namespace photos {

struct EditTarget {
	int64_t base = 0;		// the version the edit is made on; 0: the original
	int64_t replace = 0;	// the edit version to change in place; 0: a new one
	EditParams params;		// the adjustments so far
	int baseWidth = 0;		// size of the base picture
	int baseHeight = 0;
};

// An edit of an edit goes on from the same base with the same adjustments:
// the sliders show what was set, and nothing is rendered twice. An edit of
// the original or of a PhotoCraft version starts fresh on top of it.
bool GetEditTarget(Library& library, const Photo& photo, EditTarget& target);

// Renders the edit at full size and makes it the photo's current version
// (or, when the adjustments are all neutral, goes back to the base).
bool SaveEdit(Library& library, Pictures& pictures, int64_t photoId,
	const EditParams& params, std::string& error);

// Turns a photo by quarter turns (positive: clockwise).
bool RotatePhoto(Library& library, Pictures& pictures, int64_t photoId,
	int quarterTurns, std::string& error);

// Writes the photo as it is shown to a JPEG (or, for an untouched original
// with keepOriginal, copies the original file). Returns the file written.
bool ExportPhoto(Library& library, Pictures& pictures, int64_t photoId,
	const std::string& directory, bool keepOriginal, std::string& written,
	std::string& error);

}	// namespace photos
