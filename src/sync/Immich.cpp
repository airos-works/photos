#include "Immich.h"

#include "nlohmann/json.hpp"

#include <cstdlib>

using nlohmann::json;

namespace photos {

namespace {

std::string
Text(const json& object, const char* key)
{
	auto found = object.find(key);
	return found != object.end() && found->is_string()
		? found->get<std::string>() : std::string();
}


double
Number(const json& object, const char* key)
{
	auto found = object.find(key);
	if (found == object.end())
		return 0;
	if (found->is_number())
		return found->get<double>();
	if (found->is_string())
		return atof(found->get<std::string>().c_str());
	return 0;
}


bool
Flag(const json& object, const char* key)
{
	auto found = object.find(key);
	return found != object.end() && found->is_boolean() && found->get<bool>();
}


// "2022-09-22T14:01:01.000Z" read as the wall time it spells.
int64_t
WallTime(const json& object, const char* key)
{
	CivilTime civil;
	std::string text = Text(object, key);
	if (text.empty() || !ParseDateTime(text, civil))
		return 0;
	return CivilToSeconds(civil);
}

}	// namespace


double
ParseExposure(const std::string& text)
{
	size_t slash = text.find('/');
	if (slash == std::string::npos)
		return atof(text.c_str());
	double numerator = atof(text.substr(0, slash).c_str());
	double denominator = atof(text.substr(slash + 1).c_str());
	return denominator > 0 ? numerator / denominator : 0;
}


bool
ParseSyncLine(const std::string& line, SyncEvent& event)
{
	json parsed = json::parse(line, nullptr, false);
	if (!parsed.is_object())
		return false;
	event = SyncEvent();
	event.type = Text(parsed, "type");
	event.ack = Text(parsed, "ack");
	auto found = parsed.find("data");
	json empty = json::object();
	const json& data = found != parsed.end() && found->is_object() ? *found : empty;

	if (event.type == "AssetV2" || event.type == "AssetV1") {
		event.kind = SyncEvent::kAsset;
		RemoteAsset& asset = event.asset;
		asset.id = Text(data, "id");
		asset.name = Text(data, "originalFileName");
		asset.checksum = Text(data, "checksum");
		asset.thumbhash = Text(data, "thumbhash");
		asset.taken = WallTime(data, "localDateTime");
		if (asset.taken == 0)
			asset.taken = WallTime(data, "fileCreatedAt");
		asset.width = (int)Number(data, "width");
		asset.height = (int)Number(data, "height");
		asset.favorite = Flag(data, "isFavorite");
		asset.trashed = !Text(data, "deletedAt").empty();
		asset.isImage = Text(data, "type") == "IMAGE";
		asset.uploaded = WallTime(data, "createdAt");
		std::string visibility = Text(data, "visibility");
		event.hidden = visibility == "hidden" || visibility == "locked";
	} else if (event.type == "AssetDeleteV1") {
		event.kind = SyncEvent::kAssetDelete;
		event.assetId = Text(data, "assetId");
	} else if (event.type == "AssetExifV1") {
		event.kind = SyncEvent::kExif;
		RemoteExif& exif = event.exif;
		exif.assetId = Text(data, "assetId");
		exif.size = (int64_t)Number(data, "fileSizeInByte");
		exif.make = Text(data, "make");
		exif.model = Text(data, "model");
		exif.lens = Text(data, "lensModel");
		exif.iso = Number(data, "iso");
		exif.exposureTime = ParseExposure(Text(data, "exposureTime"));
		exif.fNumber = Number(data, "fNumber");
		exif.focalLength = Number(data, "focalLength");
		auto latitude = data.find("latitude");
		auto longitude = data.find("longitude");
		if (latitude != data.end() && latitude->is_number()
			&& longitude != data.end() && longitude->is_number()) {
			exif.hasGps = true;
			exif.latitude = latitude->get<double>();
			exif.longitude = longitude->get<double>();
		}
		exif.width = (int)Number(data, "exifImageWidth");
		exif.height = (int)Number(data, "exifImageHeight");
		exif.orientation = (int)Number(data, "orientation");
	} else if (event.type == "SyncResetV1")
		event.kind = SyncEvent::kReset;
	else if (event.type == "SyncCompleteV1")
		event.kind = SyncEvent::kComplete;
	return true;
}


ImmichClient::ImmichClient(Http& http)
	:
	fHttp(http),
	fMajor(0),
	fUnauthorized(false)
{
}


void
ImmichClient::SetServer(const std::string& url)
{
	fServer = Trim(url);
	while (!fServer.empty() && fServer.back() == '/')
		fServer.pop_back();
	if (EndsWith(fServer, "/api"))
		fServer.resize(fServer.size() - 4);
	if (!fServer.empty() && fServer.find("://") == std::string::npos)
		fServer = "http://" + fServer;
	fUnauthorized = false;
}


HttpRequest
ImmichClient::_Request(const std::string& method, const std::string& path) const
{
	HttpRequest request;
	request.method = method;
	request.url = fServer + "/api" + path;
	if (!fApiKey.empty())
		request.headers.emplace_back("x-api-key", fApiKey);
	else
		request.bearer = fToken;
	request.headers.emplace_back("Accept", "application/json");
	return request;
}


bool
ImmichClient::_Check(const HttpResponse& response, std::string& error)
{
	if (response.Ok())
		return true;
	if (response.status == 0) {
		error = response.error.empty() ? "no answer from the server" : response.error;
		return false;
	}
	if (response.status == 401)
		fUnauthorized = true;
	json parsed = json::parse(response.body, nullptr, false);
	std::string message;
	if (parsed.is_object()) {
		auto found = parsed.find("message");
		if (found != parsed.end() && found->is_string())
			message = found->get<std::string>();
		else if (found != parsed.end() && found->is_array() && !found->empty()
			&& (*found)[0].is_string())
			message = (*found)[0].get<std::string>();
	}
	if (response.status == 401)
		error = message.empty() ? "the server does not accept the sign-in" : message;
	else {
		error = "the server answered " + std::to_string(response.status);
		if (!message.empty())
			error += ": " + message;
	}
	return false;
}


bool
ImmichClient::Login(const std::string& email, const std::string& password,
	std::string& error)
{
	HttpRequest request;
	request.method = "POST";
	request.url = fServer + "/api/auth/login";
	request.contentType = "application/json";
	request.body = json{{"email", email}, {"password", password}}.dump();
	request.timeout = 30;
	HttpResponse response = fHttp.Perform(request);
	if (!_Check(response, error)) {
		if (response.status == 401 || response.status == 400)
			error = "wrong e-mail address or password";
		return false;
	}
	json parsed = json::parse(response.body, nullptr, false);
	std::string token = parsed.is_object() ? Text(parsed, "accessToken") : "";
	if (token.empty()) {
		error = "the server gave no session";
		return false;
	}
	fToken = token;
	fApiKey.clear();
	fUserName = Text(parsed, "name");
	fUserId = Text(parsed, "userId");
	fUnauthorized = false;
	return true;
}


void
ImmichClient::Logout()
{
	if (!fToken.empty() && !fServer.empty()) {
		HttpRequest request = _Request("POST", "/auth/logout");
		request.timeout = 10;
		fHttp.Perform(request);
	}
	fToken.clear();
}


bool
ImmichClient::Validate(std::string& error)
{
	if (fServer.empty()) {
		error = "no server";
		return false;
	}
	HttpRequest request = _Request("GET", "/users/me");
	request.timeout = 20;
	HttpResponse response = fHttp.Perform(request);
	if (!_Check(response, error))
		return false;
	json parsed = json::parse(response.body, nullptr, false);
	if (!parsed.is_object() || Text(parsed, "id").empty()) {
		error = "this does not look like an Immich server";
		return false;
	}
	fUserName = Text(parsed, "name");
	fUserId = Text(parsed, "id");
	fUnauthorized = false;

	request = _Request("GET", "/server/version");
	request.timeout = 20;
	response = fHttp.Perform(request);
	parsed = json::parse(response.body, nullptr, false);
	if (response.Ok() && parsed.is_object()) {
		fMajor = (int)Number(parsed, "major");
		fVersion = std::to_string(fMajor) + "." + std::to_string((int)Number(parsed, "minor"))
			+ "." + std::to_string((int)Number(parsed, "patch"));
	}
	return true;
}


bool
ImmichClient::SyncStream(bool reset, const SyncHandler& handler, std::string& error,
	const std::atomic<bool>* cancelled)
{
	HttpRequest request = _Request("POST", "/sync/stream");
	request.headers.back().second = "application/jsonlines+json";
	request.contentType = "application/json";
	json body = {{"types", {"AssetsV2", "AssetExifsV1"}}};
	if (reset)
		body["reset"] = true;
	request.body = body.dump();
	request.longTransfer = true;

	std::string pending;
	bool stopped = false;
	request.onBody = [&](const char* data, size_t size) {
		pending.append(data, size);
		size_t start = 0;
		for (;;) {
			size_t end = pending.find('\n', start);
			if (end == std::string::npos)
				break;
			if (end > start) {
				SyncEvent event;
				if (ParseSyncLine(pending.substr(start, end - start), event)
					&& !handler(event)) {
					stopped = true;
					return false;
				}
			}
			start = end + 1;
		}
		pending.erase(0, start);
		return true;
	};
	HttpResponse response = fHttp.Perform(request, cancelled);
	if (stopped)
		return true;
	if (!_Check(response, error))
		return false;
	if (!Trim(pending).empty()) {
		SyncEvent event;
		if (ParseSyncLine(pending, event))
			handler(event);
	}
	return true;
}


bool
ImmichClient::Acknowledge(const std::vector<std::string>& acks, std::string& error)
{
	if (acks.empty())
		return true;
	HttpRequest request = _Request("POST", "/sync/ack");
	request.contentType = "application/json";
	request.body = json{{"acks", acks}}.dump();
	return _Check(fHttp.Perform(request), error);
}


bool
ImmichClient::DownloadThumbnail(const std::string& assetId, const char* size,
	const std::string& path, std::string& error, const std::atomic<bool>* cancelled)
{
	HttpRequest request = _Request("GET",
		"/assets/" + assetId + "/thumbnail?size=" + size);
	request.headers.back().second = "*/*";
	request.downloadPath = path;
	request.timeout = 120;
	return _Check(fHttp.Perform(request, cancelled), error);
}


bool
ImmichClient::DownloadOriginal(const std::string& assetId, const std::string& path,
	std::string& error, const std::atomic<bool>* cancelled)
{
	HttpRequest request = _Request("GET", "/assets/" + assetId + "/original");
	request.headers.back().second = "*/*";
	request.downloadPath = path;
	request.longTransfer = true;
	return _Check(fHttp.Perform(request, cancelled), error);
}


bool
ImmichClient::CheckUploads(std::vector<UploadCheck>& checks, std::string& error)
{
	if (checks.empty())
		return true;
	json assets = json::array();
	for (auto& check : checks)
		assets.push_back({{"id", check.key}, {"checksum", check.checksum}});
	HttpRequest request = _Request("POST", "/assets/bulk-upload-check");
	request.contentType = "application/json";
	request.body = json{{"assets", assets}}.dump();
	HttpResponse response = fHttp.Perform(request);
	if (!_Check(response, error))
		return false;
	json parsed = json::parse(response.body, nullptr, false);
	if (!parsed.is_object() || !parsed.contains("results")
		|| !parsed["results"].is_array()) {
		error = "unexpected answer to the upload check";
		return false;
	}
	for (auto& result : parsed["results"]) {
		if (!result.is_object())
			continue;
		std::string key = Text(result, "id");
		for (auto& check : checks) {
			if (check.key != key)
				continue;
			check.accept = Text(result, "action") == "accept";
			check.unsupported = Text(result, "reason") == "unsupported-format";
			check.existingId = Text(result, "assetId");
			check.existingTrashed = Flag(result, "isTrashed");
			break;
		}
	}
	return true;
}


bool
ImmichClient::Upload(const std::string& path, const std::string& name,
	const std::string& checksum, int64_t created, int64_t modified, bool favorite,
	std::string& assetId, bool& duplicate, std::string& error,
	const std::atomic<bool>* cancelled)
{
	HttpRequest request = _Request("POST", "/assets");
	if (!checksum.empty())
		request.headers.emplace_back("x-immich-checksum", checksum);
	request.longTransfer = true;
	auto field = [&](const char* fieldName, const std::string& value) {
		HttpPart part;
		part.name = fieldName;
		part.value = value;
		request.multipart.push_back(part);
	};
	if (fMajor > 0 && fMajor < 3) {
		// Servers before 3.0 want the device's name for the file.
		field("deviceAssetId", name + "-" + std::to_string(FileSize(path)));
		field("deviceId", "airos-photos");
	}
	field("fileCreatedAt", FormatIso(created, true));
	field("fileModifiedAt", FormatIso(modified, true));
	field("filename", name);
	if (favorite)
		field("isFavorite", "true");
	HttpPart file;
	file.name = "assetData";
	file.filePath = path;
	file.fileName = name;
	file.contentType = "application/octet-stream";
	request.multipart.push_back(file);

	HttpResponse response = fHttp.Perform(request, cancelled);
	if (!_Check(response, error))
		return false;
	json parsed = json::parse(response.body, nullptr, false);
	assetId = parsed.is_object() ? Text(parsed, "id") : "";
	if (assetId.empty()) {
		error = "the server did not name the new asset";
		return false;
	}
	duplicate = Text(parsed, "status") == "duplicate";
	return true;
}


bool
ImmichClient::SetFavorite(const std::vector<std::string>& assetIds, bool favorite,
	std::string& error)
{
	if (assetIds.empty())
		return true;
	HttpRequest request = _Request("PUT", "/assets");
	request.contentType = "application/json";
	request.body = json{{"ids", assetIds}, {"isFavorite", favorite}}.dump();
	return _Check(fHttp.Perform(request), error);
}


bool
ImmichClient::TrashAssets(const std::vector<std::string>& assetIds, std::string& error)
{
	if (assetIds.empty())
		return true;
	HttpRequest request = _Request("DELETE", "/assets");
	request.contentType = "application/json";
	request.body = json{{"ids", assetIds}, {"force", false}}.dump();
	return _Check(fHttp.Perform(request), error);
}

}	// namespace photos
