#include "Settings.h"

#include "Util.h"

#include "nlohmann/json.hpp"

#include <sys/stat.h>

using nlohmann::json;

namespace photos {

namespace setting {
const char* const kLibraryPath = "library.path";
const char* const kImmichEnabled = "immich.enabled";
const char* const kImmichServer = "immich.server";
const char* const kImmichUser = "immich.user";
const char* const kImmichToken = "immich.token";
const char* const kImmichApiKey = "immich.api_key";
const char* const kImmichUpload = "immich.upload";
const char* const kImmichKeepOriginals = "immich.keep_originals";
const char* const kImmichTrashOnServer = "immich.trash_on_server";
const char* const kImmichInterval = "immich.interval";
const char* const kFacesEnabled = "faces.enabled";
const char* const kFacesCpuOnly = "faces.cpu_only";
const char* const kRawUsePreview = "raw.use_preview";
const char* const kOpenOnConnect = "devices.open_on_connect";
const char* const kDeleteAfterImport = "devices.delete_after_import";
const char* const kTileSize = "ui.tile_size";
const char* const kTrashDays = "trash.days";
}

struct Settings::Data {
	json values = json::object();
};


Settings::Settings()
	:
	fData(new Data)
{
}


Settings::~Settings()
{
	delete fData;
}


void
Settings::Load(const std::string& path)
{
	std::lock_guard<std::mutex> lock(fLock);
	fPath = path;
	fData->values = json::object();
	std::string text;
	if (ReadFile(path, text, 4 << 20)) {
		json parsed = json::parse(text, nullptr, false);
		if (parsed.is_object())
			fData->values = parsed;
	}
}


bool
Settings::Save()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (fPath.empty())
		return false;
	MakeDirs(DirName(fPath));
	std::string text = fData->values.dump(1, '\t');
	if (!WriteFile(fPath, text.data(), text.size()))
		return false;
	// The file holds the Immich session: the user's eyes only.
	chmod(fPath.c_str(), 0600);
	return true;
}


std::string
Settings::GetString(const char* key, const std::string& fallback)
{
	std::lock_guard<std::mutex> lock(fLock);
	auto found = fData->values.find(key);
	return found != fData->values.end() && found->is_string()
		? found->get<std::string>() : fallback;
}


int64_t
Settings::GetInt(const char* key, int64_t fallback)
{
	std::lock_guard<std::mutex> lock(fLock);
	auto found = fData->values.find(key);
	return found != fData->values.end() && found->is_number()
		? found->get<int64_t>() : fallback;
}


double
Settings::GetDouble(const char* key, double fallback)
{
	std::lock_guard<std::mutex> lock(fLock);
	auto found = fData->values.find(key);
	return found != fData->values.end() && found->is_number()
		? found->get<double>() : fallback;
}


bool
Settings::GetBool(const char* key, bool fallback)
{
	std::lock_guard<std::mutex> lock(fLock);
	auto found = fData->values.find(key);
	return found != fData->values.end() && found->is_boolean()
		? found->get<bool>() : fallback;
}


void
Settings::SetString(const char* key, const std::string& value)
{
	std::lock_guard<std::mutex> lock(fLock);
	fData->values[key] = value;
}


void
Settings::SetInt(const char* key, int64_t value)
{
	std::lock_guard<std::mutex> lock(fLock);
	fData->values[key] = value;
}


void
Settings::SetDouble(const char* key, double value)
{
	std::lock_guard<std::mutex> lock(fLock);
	fData->values[key] = value;
}


void
Settings::SetBool(const char* key, bool value)
{
	std::lock_guard<std::mutex> lock(fLock);
	fData->values[key] = value;
}


void
Settings::Remove(const char* key)
{
	std::lock_guard<std::mutex> lock(fLock);
	fData->values.erase(key);
}

}	// namespace photos
