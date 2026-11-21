// Small portable helpers: hashing, text, time, files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace photos {

// --- SHA-1 (Immich identifies assets by the SHA-1 of the file) ---------------

class Sha1 {
public:
	Sha1();
	void Update(const void* data, size_t size);
	void Finish(uint8_t digest[20]);

private:
	void _Block(const uint8_t* block);

	uint32_t fState[5];
	uint64_t fLength;
	uint8_t fBuffer[64];
	size_t fFill;
};

// Base64 of the SHA-1 of a file (the form Immich uses), "" when unreadable.
std::string Sha1OfFile(const std::string& path);
std::string Sha1OfData(const void* data, size_t size);
std::string HexToBase64(const std::string& hex);
std::string Base64Decode(const std::string& text);

// --- Text -----------------------------------------------------------------

std::string ToLower(std::string text);
std::string Trim(const std::string& text);
bool StartsWith(const std::string& text, const std::string& prefix);
bool EndsWith(const std::string& text, const std::string& suffix);
// "IMG_1234.JPG" -> "jpg" (lower case, no dot); "" without one.
std::string Extension(const std::string& name);
std::string BaseName(const std::string& path);			// last component
std::string DirName(const std::string& path);
std::string StripExtension(const std::string& name);
std::string JoinPath(const std::string& a, const std::string& b);
std::string FormatSize(int64_t bytes);					// "3.4 MB"
std::string RandomId();									// 32 hex digits

// --- Kinds of files ---------------------------------------------------------

enum MediaKind {
	kKindUnknown = 0,
	kKindJpeg,
	kKindPng,
	kKindHeic,
	kKindRaw,
	kKindOther,		// decoded by a translator: TIFF, WebP, AVIF, GIF, BMP...
	kKindVideo
};

MediaKind KindOfName(const std::string& name);
bool IsPictureName(const std::string& name);	// anything but unknown and video
const char* KindName(MediaKind kind);

// --- Time -------------------------------------------------------------------
// The library keeps a photo's capture time as "local wall time": seconds since
// 1970 of the date and time the camera's clock showed, with no zone. Days and
// months of the timeline are those of the place the picture was taken.

struct CivilTime {
	int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

int64_t CivilToSeconds(const CivilTime& time);
CivilTime SecondsToCivil(int64_t seconds);
int DayOfWeek(int64_t seconds);				// 0 = Sunday
// "2024-03-09T14:05:33.120Z", "2024:03:09 14:05:33", "20240309T140533":
// the fields as written (the zone is not applied). False when unparsable.
bool ParseDateTime(const std::string& text, CivilTime& time);
// Seconds east of UTC from a trailing "Z", "+10:00", "-0330"; false if none.
bool ParseZoneOffset(const std::string& text, int& seconds);
std::string FormatIso(int64_t seconds, bool utcSuffix);	// 2024-03-09T14:05:33.000Z
int64_t NowSeconds();						// UTC
int64_t LocalOffsetSeconds(int64_t utc);	// of this machine's zone

// --- Files ------------------------------------------------------------------

bool FileExists(const std::string& path);
int64_t FileSize(const std::string& path);		// -1 when missing
int64_t FileModified(const std::string& path);	// seconds UTC, 0 when missing
bool MakeDirs(const std::string& path);
bool CopyFile(const std::string& from, const std::string& to);	// via a temporary, atomic
bool ReadFile(const std::string& path, std::string& data, size_t limit = SIZE_MAX);
bool WriteFile(const std::string& path, const void* data, size_t size);	// atomic
bool RemoveFile(const std::string& path);
bool RemoveTree(const std::string& path);
// A name in `directory` that is free: "name.jpg", "name 2.jpg", ...
std::string UniqueName(const std::string& directory, const std::string& name);
void ListDirectory(const std::string& path, std::vector<std::string>& names);

}	// namespace photos
