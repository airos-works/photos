#include "Util.h"

#include "Http.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace photos {

// --- SHA-1 ------------------------------------------------------------------

static inline uint32_t
Rol(uint32_t value, int bits)
{
	return (value << bits) | (value >> (32 - bits));
}


Sha1::Sha1()
	:
	fLength(0),
	fFill(0)
{
	fState[0] = 0x67452301;
	fState[1] = 0xefcdab89;
	fState[2] = 0x98badcfe;
	fState[3] = 0x10325476;
	fState[4] = 0xc3d2e1f0;
}


void
Sha1::_Block(const uint8_t* block)
{
	uint32_t w[80];
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16
			| (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
	}
	for (int i = 16; i < 80; i++)
		w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = fState[0], b = fState[1], c = fState[2], d = fState[3],
		e = fState[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		uint32_t t = Rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = Rol(b, 30);
		b = a;
		a = t;
	}
	fState[0] += a;
	fState[1] += b;
	fState[2] += c;
	fState[3] += d;
	fState[4] += e;
}


void
Sha1::Update(const void* data, size_t size)
{
	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	fLength += size;
	if (fFill > 0) {
		size_t take = std::min(size, sizeof(fBuffer) - fFill);
		memcpy(fBuffer + fFill, bytes, take);
		fFill += take;
		bytes += take;
		size -= take;
		if (fFill < sizeof(fBuffer))
			return;
		_Block(fBuffer);
		fFill = 0;
	}
	while (size >= 64) {
		_Block(bytes);
		bytes += 64;
		size -= 64;
	}
	if (size > 0) {
		memcpy(fBuffer, bytes, size);
		fFill = size;
	}
}


void
Sha1::Finish(uint8_t digest[20])
{
	uint64_t bits = fLength * 8;
	uint8_t pad = 0x80;
	Update(&pad, 1);
	uint8_t zero = 0;
	while (fFill != 56)
		Update(&zero, 1);
	uint8_t length[8];
	for (int i = 0; i < 8; i++)
		length[i] = (uint8_t)(bits >> (56 - i * 8));
	Update(length, 8);
	for (int i = 0; i < 5; i++) {
		digest[i * 4] = (uint8_t)(fState[i] >> 24);
		digest[i * 4 + 1] = (uint8_t)(fState[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(fState[i] >> 8);
		digest[i * 4 + 3] = (uint8_t)fState[i];
	}
}


std::string
Sha1OfFile(const std::string& path)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return std::string();
	Sha1 sha;
	std::vector<uint8_t> buffer(1 << 18);
	size_t got;
	while ((got = fread(buffer.data(), 1, buffer.size(), file)) > 0)
		sha.Update(buffer.data(), got);
	bool failed = ferror(file) != 0;
	fclose(file);
	if (failed)
		return std::string();
	uint8_t digest[20];
	sha.Finish(digest);
	return Base64Encode(std::string((const char*)digest, 20));
}


std::string
Sha1OfData(const void* data, size_t size)
{
	Sha1 sha;
	sha.Update(data, size);
	uint8_t digest[20];
	sha.Finish(digest);
	return Base64Encode(std::string((const char*)digest, 20));
}


std::string
HexToBase64(const std::string& hex)
{
	std::string bytes;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		auto digit = [](char c) -> int {
			if (c >= '0' && c <= '9')
				return c - '0';
			c = (char)tolower((unsigned char)c);
			return c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0;
		};
		bytes.push_back((char)(digit(hex[i]) << 4 | digit(hex[i + 1])));
	}
	return Base64Encode(bytes);
}


std::string
Base64Decode(const std::string& text)
{
	std::string out;
	uint32_t value = 0;
	int bits = 0;
	for (unsigned char c : text) {
		int digit;
		if (c >= 'A' && c <= 'Z')
			digit = c - 'A';
		else if (c >= 'a' && c <= 'z')
			digit = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			digit = c - '0' + 52;
		else if (c == '+' || c == '-')
			digit = 62;
		else if (c == '/' || c == '_')
			digit = 63;
		else
			continue;
		value = value << 6 | (uint32_t)digit;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((char)(value >> bits & 0xff));
		}
	}
	return out;
}


// --- Text -------------------------------------------------------------------

std::string
ToLower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char c) { return (char)tolower(c); });
	return text;
}


std::string
Trim(const std::string& text)
{
	size_t begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return std::string();
	size_t end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}


bool
StartsWith(const std::string& text, const std::string& prefix)
{
	return text.size() >= prefix.size()
		&& text.compare(0, prefix.size(), prefix) == 0;
}


bool
EndsWith(const std::string& text, const std::string& suffix)
{
	return text.size() >= suffix.size()
		&& text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}


std::string
BaseName(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}


std::string
DirName(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	if (slash == std::string::npos)
		return ".";
	return slash == 0 ? "/" : path.substr(0, slash);
}


std::string
Extension(const std::string& name)
{
	std::string base = BaseName(name);
	size_t dot = base.find_last_of('.');
	if (dot == std::string::npos || dot == 0)
		return std::string();
	return ToLower(base.substr(dot + 1));
}


std::string
StripExtension(const std::string& name)
{
	size_t slash = name.find_last_of('/');
	size_t dot = name.find_last_of('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash)
		|| dot == (slash == std::string::npos ? 0 : slash + 1))
		return name;
	return name.substr(0, dot);
}


std::string
JoinPath(const std::string& a, const std::string& b)
{
	if (a.empty())
		return b;
	if (b.empty())
		return a;
	if (a.back() == '/')
		return a + (b.front() == '/' ? b.substr(1) : b);
	return a + (b.front() == '/' ? b : "/" + b);
}


std::string
FormatSize(int64_t bytes)
{
	char text[64];
	if (bytes < 1000)
		snprintf(text, sizeof(text), "%d bytes", (int)bytes);
	else if (bytes < 1000 * 1000)
		snprintf(text, sizeof(text), "%.0f KB", bytes / 1e3);
	else if (bytes < 1000LL * 1000 * 1000)
		snprintf(text, sizeof(text), "%.1f MB", bytes / 1e6);
	else
		snprintf(text, sizeof(text), "%.2f GB", bytes / 1e9);
	return text;
}


std::string
RandomId()
{
	static std::mt19937_64 generator{std::random_device{}()
		^ (uint64_t)time(nullptr) << 20 ^ (uint64_t)getpid()};
	static const char* kDigits = "0123456789abcdef";
	std::string id;
	for (int i = 0; i < 2; i++) {
		uint64_t value = generator();
		for (int j = 0; j < 16; j++) {
			id.push_back(kDigits[value & 15]);
			value >>= 4;
		}
	}
	return id;
}


// --- Kinds ------------------------------------------------------------------

MediaKind
KindOfName(const std::string& name)
{
	static const char* const kRaw[] = {"dng", "cr2", "cr3", "crw", "nef",
		"nrw", "arw", "srf", "sr2", "raf", "rw2", "rwl", "orf", "pef", "srw",
		"3fr", "erf", "kdc", "dcr", "mos", "mrw", "x3f", "iiq", nullptr};
	static const char* const kOther[] = {"tif", "tiff", "webp", "avif",
		"gif", "bmp", "jp2", "j2k", "jxl", "psd", "tga", nullptr};
	static const char* const kVideo[] = {"mov", "mp4", "m4v", "avi", "mts",
		"m2ts", "3gp", "mkv", "webm", "mpg", "mpeg", "wmv", nullptr};
	std::string ext = Extension(name);
	if (ext.empty())
		return kKindUnknown;
	if (ext == "jpg" || ext == "jpeg" || ext == "jpe")
		return kKindJpeg;
	if (ext == "png")
		return kKindPng;
	if (ext == "heic" || ext == "heif" || ext == "hif")
		return kKindHeic;
	for (int i = 0; kRaw[i] != nullptr; i++) {
		if (ext == kRaw[i])
			return kKindRaw;
	}
	for (int i = 0; kOther[i] != nullptr; i++) {
		if (ext == kOther[i])
			return kKindOther;
	}
	for (int i = 0; kVideo[i] != nullptr; i++) {
		if (ext == kVideo[i])
			return kKindVideo;
	}
	return kKindUnknown;
}


bool
IsPictureName(const std::string& name)
{
	MediaKind kind = KindOfName(name);
	return kind != kKindUnknown && kind != kKindVideo;
}


const char*
KindName(MediaKind kind)
{
	switch (kind) {
		case kKindJpeg: return "JPEG";
		case kKindPng: return "PNG";
		case kKindHeic: return "HEIF";
		case kKindRaw: return "RAW";
		case kKindOther: return "Image";
		case kKindVideo: return "Video";
		default: return "File";
	}
}


// --- Time -------------------------------------------------------------------

// Days from 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
// algorithm).
static int64_t
DaysFromCivil(int64_t year, int month, int day)
{
	year -= month <= 2;
	int64_t era = (year >= 0 ? year : year - 399) / 400;
	int64_t yoe = year - era * 400;
	int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}


int64_t
CivilToSeconds(const CivilTime& time)
{
	return DaysFromCivil(time.year, time.month, time.day) * 86400
		+ time.hour * 3600 + time.minute * 60 + time.second;
}


CivilTime
SecondsToCivil(int64_t seconds)
{
	int64_t days = seconds >= 0 ? seconds / 86400 : -((-seconds + 86399) / 86400);
	int64_t rest = seconds - days * 86400;
	days += 719468;
	int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	int64_t doe = days - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t year = yoe + era * 400;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	CivilTime time;
	time.day = (int)(doy - (153 * mp + 2) / 5 + 1);
	time.month = (int)(mp < 10 ? mp + 3 : mp - 9);
	time.year = (int)(year + (time.month <= 2));
	time.hour = (int)(rest / 3600);
	time.minute = (int)(rest % 3600 / 60);
	time.second = (int)(rest % 60);
	return time;
}


int
DayOfWeek(int64_t seconds)
{
	int64_t days = seconds >= 0 ? seconds / 86400 : -((-seconds + 86399) / 86400);
	int day = (int)((days + 4) % 7);
	return day < 0 ? day + 7 : day;
}


bool
ParseDateTime(const std::string& text, CivilTime& time)
{
	// Collect the runs of digits: either separated fields or the compact
	// "YYYYMMDDThhmmss" of PTP.
	int fields[6] = {0, 1, 1, 0, 0, 0};
	int count = 0;
	size_t i = 0;
	size_t length = text.size();
	while (i < length && count < 6) {
		if (!isdigit((unsigned char)text[i])) {
			// A zone or fraction ends the fields once the time is complete.
			if (count >= 3 && (text[i] == 'Z' || text[i] == '+'
					|| (text[i] == '.' && count == 6)))
				break;
			if (count >= 5 && text[i] == '-')
				break;
			i++;
			continue;
		}
		size_t start = i;
		while (i < length && isdigit((unsigned char)text[i]))
			i++;
		size_t run = i - start;
		if (count == 0 && run == 8) {
			fields[0] = atoi(text.substr(start, 4).c_str());
			fields[1] = atoi(text.substr(start + 4, 2).c_str());
			fields[2] = atoi(text.substr(start + 6, 2).c_str());
			count = 3;
		} else if (count == 3 && run == 6) {
			fields[3] = atoi(text.substr(start, 2).c_str());
			fields[4] = atoi(text.substr(start + 2, 2).c_str());
			fields[5] = atoi(text.substr(start + 4, 2).c_str());
			count = 6;
		} else {
			if (run > 4)
				return false;
			fields[count++] = atoi(text.substr(start, run).c_str());
			if (count == 6)
				break;
		}
	}
	if (count < 3)
		return false;
	if (fields[0] < 1800 || fields[0] > 9999 || fields[1] < 1 || fields[1] > 12
		|| fields[2] < 1 || fields[2] > 31 || fields[3] > 23 || fields[4] > 59
		|| fields[5] > 60)
		return false;
	time.year = fields[0];
	time.month = fields[1];
	time.day = fields[2];
	time.hour = fields[3];
	time.minute = fields[4];
	time.second = std::min(fields[5], 59);
	return true;
}


bool
ParseZoneOffset(const std::string& text, int& seconds)
{
	std::string trimmed = Trim(text);
	if (trimmed.empty())
		return false;
	if (trimmed.back() == 'Z' || trimmed.back() == 'z') {
		seconds = 0;
		return true;
	}
	// The last '+' or '-' after the time's "T" or a space.
	size_t mark = trimmed.find_last_of("+-");
	size_t timeStart = trimmed.find_first_of("T ");
	if (mark == std::string::npos
		|| (trimmed.size() > 6 && (timeStart == std::string::npos || mark < timeStart)))
		return false;
	std::string digits;
	for (size_t i = mark + 1; i < trimmed.size(); i++) {
		if (isdigit((unsigned char)trimmed[i]))
			digits.push_back(trimmed[i]);
		else if (trimmed[i] != ':')
			return false;
	}
	if (digits.size() != 4 && digits.size() != 2)
		return false;
	int hours = atoi(digits.substr(0, 2).c_str());
	int minutes = digits.size() == 4 ? atoi(digits.substr(2, 2).c_str()) : 0;
	if (hours > 14 || minutes > 59)
		return false;
	seconds = (hours * 3600 + minutes * 60) * (trimmed[mark] == '-' ? -1 : 1);
	return true;
}


std::string
FormatIso(int64_t seconds, bool utcSuffix)
{
	CivilTime time = SecondsToCivil(seconds);
	char text[64];
	snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.000%s",
		time.year, time.month, time.day, time.hour, time.minute, time.second,
		utcSuffix ? "Z" : "");
	return text;
}


int64_t
NowSeconds()
{
	return (int64_t)time(nullptr);
}


int64_t
LocalOffsetSeconds(int64_t utc)
{
	time_t value = (time_t)utc;
	struct tm local;
	localtime_r(&value, &local);
	CivilTime civil;
	civil.year = local.tm_year + 1900;
	civil.month = local.tm_mon + 1;
	civil.day = local.tm_mday;
	civil.hour = local.tm_hour;
	civil.minute = local.tm_min;
	civil.second = local.tm_sec;
	return CivilToSeconds(civil) - utc;
}


// --- Files ------------------------------------------------------------------

bool
FileExists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}


int64_t
FileSize(const std::string& path)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return -1;
	return (int64_t)st.st_size;
}


int64_t
FileModified(const std::string& path)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return 0;
	return (int64_t)st.st_mtime;
}


bool
MakeDirs(const std::string& path)
{
	if (path.empty())
		return false;
	struct stat st;
	if (stat(path.c_str(), &st) == 0)
		return S_ISDIR(st.st_mode);
	std::string parent = DirName(path);
	if (parent != path && parent != "." && parent != "/" && !MakeDirs(parent))
		return false;
	return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}


bool
CopyFile(const std::string& from, const std::string& to)
{
	FILE* in = fopen(from.c_str(), "rb");
	if (in == nullptr)
		return false;
	std::string temporary = to + ".part";
	FILE* out = fopen(temporary.c_str(), "wb");
	if (out == nullptr) {
		fclose(in);
		return false;
	}
	std::vector<char> buffer(1 << 18);
	bool ok = true;
	size_t got;
	while ((got = fread(buffer.data(), 1, buffer.size(), in)) > 0) {
		if (fwrite(buffer.data(), 1, got, out) != got) {
			ok = false;
			break;
		}
	}
	ok = ok && ferror(in) == 0;
	fclose(in);
	ok = fclose(out) == 0 && ok;
	if (ok)
		ok = rename(temporary.c_str(), to.c_str()) == 0;
	if (!ok)
		unlink(temporary.c_str());
	return ok;
}


bool
ReadFile(const std::string& path, std::string& data, size_t limit)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	data.clear();
	char buffer[1 << 16];
	size_t got;
	while (data.size() < limit
		&& (got = fread(buffer, 1, std::min(sizeof(buffer), limit - data.size()),
			file)) > 0)
		data.append(buffer, got);
	bool ok = ferror(file) == 0;
	fclose(file);
	return ok;
}


bool
WriteFile(const std::string& path, const void* data, size_t size)
{
	std::string temporary = path + ".part";
	FILE* file = fopen(temporary.c_str(), "wb");
	if (file == nullptr)
		return false;
	bool ok = size == 0 || fwrite(data, 1, size, file) == size;
	ok = fclose(file) == 0 && ok;
	if (ok)
		ok = rename(temporary.c_str(), path.c_str()) == 0;
	if (!ok)
		unlink(temporary.c_str());
	return ok;
}


bool
RemoveFile(const std::string& path)
{
	return unlink(path.c_str()) == 0 || errno == ENOENT;
}


bool
RemoveTree(const std::string& path)
{
	struct stat st;
	if (lstat(path.c_str(), &st) != 0)
		return errno == ENOENT;
	if (!S_ISDIR(st.st_mode))
		return unlink(path.c_str()) == 0;
	std::vector<std::string> names;
	ListDirectory(path, names);
	bool ok = true;
	for (auto& name : names)
		ok = RemoveTree(JoinPath(path, name)) && ok;
	return rmdir(path.c_str()) == 0 && ok;
}


std::string
UniqueName(const std::string& directory, const std::string& name)
{
	if (!FileExists(JoinPath(directory, name)))
		return name;
	std::string stem = StripExtension(name);
	std::string ext = name.substr(stem.size());
	for (int i = 2; i < 100000; i++) {
		std::string candidate = stem + " " + std::to_string(i) + ext;
		if (!FileExists(JoinPath(directory, candidate)))
			return candidate;
	}
	return RandomId() + ext;
}


void
ListDirectory(const std::string& path, std::vector<std::string>& names)
{
	DIR* dir = opendir(path.c_str());
	if (dir == nullptr)
		return;
	while (struct dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		names.push_back(entry->d_name);
	}
	closedir(dir);
	std::sort(names.begin(), names.end());
}

}	// namespace photos
