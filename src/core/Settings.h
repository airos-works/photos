// The application's settings: a JSON file of named values
// (~/config/settings/Photos/settings.json). Thread-safe.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace photos {

class Settings {
public:
	Settings();
	~Settings();

	// Reads the file; missing or damaged files give the defaults.
	void Load(const std::string& path);
	bool Save();
	const std::string& Path() const { return fPath; }

	std::string GetString(const char* key, const std::string& fallback = "");
	int64_t GetInt(const char* key, int64_t fallback = 0);
	double GetDouble(const char* key, double fallback = 0);
	bool GetBool(const char* key, bool fallback = false);
	void SetString(const char* key, const std::string& value);
	void SetInt(const char* key, int64_t value);
	void SetDouble(const char* key, double value);
	void SetBool(const char* key, bool value);
	void Remove(const char* key);

private:
	struct Data;
	Data* fData;
	std::string fPath;
	std::mutex fLock;
};

// The names of the settings.
namespace setting {
extern const char* const kLibraryPath;		// "" = ~/Pictures/Photos Library
extern const char* const kImmichEnabled;
extern const char* const kImmichServer;
extern const char* const kImmichUser;			// e-mail, shown only
extern const char* const kImmichToken;
extern const char* const kImmichApiKey;
extern const char* const kImmichUpload;		// default true
extern const char* const kImmichKeepOriginals;
extern const char* const kImmichTrashOnServer;
extern const char* const kImmichInterval;		// minutes, default 15
extern const char* const kFacesEnabled;		// default true
extern const char* const kFacesCpuOnly;
extern const char* const kRawUsePreview;		// show the camera's preview instead of developing
extern const char* const kOpenOnConnect;		// default true
extern const char* const kDeleteAfterImport;
extern const char* const kTileSize;
extern const char* const kTrashDays;			// default 30
}

}	// namespace photos
