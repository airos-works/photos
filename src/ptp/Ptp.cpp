#include "ptp/Ptp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace photos {

namespace {

const size_t kHeaderSize = 12;
const size_t kChunkSize = 256 * 1024;
const uint32_t kUnknownLength = 0xFFFFFFFF;
const uint16_t kTypeCommand = 1;
const uint16_t kTypeData = 2;
const uint16_t kTypeResponse = 3;

// What is thrown away, at most, to get a device back in step.
const size_t kMaxDrainBytes = 32 * 1024 * 1024;
const int kDrainTimeoutMs = 400;

const size_t kSmallDataLimit = 4 * 1024 * 1024;
const size_t kThumbLimit = 16 * 1024 * 1024;
const size_t kListLimit = 256 * 1024 * 1024;


uint16_t
Get16(const uint8_t* p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}


uint32_t
Get32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}


void
Put16(uint8_t* p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}


void
Put32(uint8_t* p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}


void
AppendUtf8(std::string& out, uint32_t c)
{
	if (c < 0x80) {
		out += (char)c;
	} else if (c < 0x800) {
		out += (char)(0xC0 | (c >> 6));
		out += (char)(0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		out += (char)(0xE0 | (c >> 12));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	} else {
		out += (char)(0xF0 | (c >> 18));
		out += (char)(0x80 | ((c >> 12) & 0x3F));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	}
}


// Reads the datasets of PTP: little endian, strings as a count of UTF-16
// units. Reading past the end makes the reader fail and gives zeros.
class Reader {
public:
	Reader(const uint8_t* data, size_t size)
		:
		fData(data),
		fSize(size),
		fPos(0),
		fOk(true)
	{
	}

	bool Ok() const { return fOk; }
	size_t Remaining() const { return fOk ? fSize - fPos : 0; }

	bool Skip(uint64_t count)
	{
		if (!fOk || count > fSize - fPos) {
			fOk = false;
			return false;
		}
		fPos += (size_t)count;
		return true;
	}

	uint8_t U8()
	{
		if (!Skip(1))
			return 0;
		return fData[fPos - 1];
	}

	uint16_t U16()
	{
		if (!Skip(2))
			return 0;
		return Get16(fData + fPos - 2);
	}

	uint32_t U32()
	{
		if (!Skip(4))
			return 0;
		return Get32(fData + fPos - 4);
	}

	uint64_t U64()
	{
		uint64_t low = U32();
		uint64_t high = U32();
		return low | (high << 32);
	}

	std::string String()
	{
		std::string text;
		uint32_t count = U8();
		if (count == 0 || !fOk)
			return text;
		if ((size_t)count * 2 > fSize - fPos) {
			fOk = false;
			return text;
		}
		for (uint32_t i = 0; i < count; i++) {
			uint32_t c = Get16(fData + fPos + i * 2);
			if (c == 0)
				break;
			if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) {
				uint32_t low = Get16(fData + fPos + (i + 1) * 2);
				if (low >= 0xDC00 && low < 0xE000) {
					c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
					i++;
				} else
					c = 0xFFFD;
			} else if (c >= 0xD800 && c < 0xE000)
				c = 0xFFFD;
			AppendUtf8(text, c);
		}
		fPos += (size_t)count * 2;
		return text;
	}

	void Array16(std::vector<uint16_t>& values)
	{
		values.clear();
		uint32_t count = U32();
		if (!fOk || (uint64_t)count * 2 > fSize - fPos) {
			fOk = false;
			return;
		}
		values.reserve(count);
		for (uint32_t i = 0; i < count; i++)
			values.push_back(U16());
	}

	void Array32(std::vector<uint32_t>& values)
	{
		values.clear();
		uint32_t count = U32();
		if (!fOk || (uint64_t)count * 4 > fSize - fPos) {
			fOk = false;
			return;
		}
		values.reserve(count);
		for (uint32_t i = 0; i < count; i++)
			values.push_back(U32());
	}

private:
	const uint8_t*	fData;
	size_t			fSize;
	size_t			fPos;
	bool			fOk;
};


bool
ParseDeviceInfo(const std::vector<uint8_t>& data, PtpDeviceInfo& info)
{
	Reader reader(data.data(), data.size());
	info = PtpDeviceInfo();
	info.standardVersion = reader.U16();
	info.vendorExtensionId = reader.U32();
	info.vendorExtensionVersion = reader.U16();
	info.vendorExtensionDescription = reader.String();
	info.functionalMode = reader.U16();
	reader.Array16(info.operations);
	reader.Array16(info.events);
	reader.Array16(info.deviceProperties);
	reader.Array16(info.captureFormats);
	reader.Array16(info.imageFormats);
	// Enough to work with; the names are for people.
	bool ok = reader.Ok();
	info.manufacturer = reader.String();
	info.model = reader.String();
	info.deviceVersion = reader.String();
	info.serialNumber = reader.String();
	return ok;
}


bool
ParseObjectInfo(const std::vector<uint8_t>& data, PtpObjectInfo& info)
{
	Reader reader(data.data(), data.size());
	info.storageId = reader.U32();
	info.format = reader.U16();
	info.protection = reader.U16();
	uint32_t size = reader.U32();
	info.size = size == 0xFFFFFFFF ? kPtpSizeUnknown : size;
	info.thumbFormat = reader.U16();
	info.thumbSize = reader.U32();
	info.thumbWidth = reader.U32();
	info.thumbHeight = reader.U32();
	info.imageWidth = reader.U32();
	info.imageHeight = reader.U32();
	reader.U32();	// bit depth
	info.parent = reader.U32();
	info.associationType = reader.U16();
	reader.U32();	// association description
	reader.U32();	// sequence number
	info.filename = reader.String();
	// The name is what a listing cannot do without.
	bool ok = reader.Ok();
	info.captured = PtpParseDate(reader.String(), &info.capturedIsUtc);
	info.modified = PtpParseDate(reader.String(), &info.modifiedIsUtc);
	return ok;
}


int64_t
DaysFromCivil(int64_t year, unsigned month, unsigned day)
{
	year -= month <= 2;
	const int64_t era = (year >= 0 ? year : year - 399) / 400;
	const unsigned yearOfEra = (unsigned)(year - era * 400);
	const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5
		+ day - 1;
	const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100
		+ dayOfYear;
	return era * 146097 + (int64_t)dayOfEra - 719468;
}


bool
Digits(const std::string& text, size_t at, size_t count, int& value)
{
	if (at + count > text.size())
		return false;
	value = 0;
	for (size_t i = 0; i < count; i++) {
		char c = text[at + i];
		if (c < '0' || c > '9')
			return false;
		value = value * 10 + (c - '0');
	}
	return true;
}


std::string
LowerExtension(const std::string& filename)
{
	size_t dot = filename.rfind('.');
	if (dot == std::string::npos || dot + 1 >= filename.size())
		return std::string();
	std::string extension = filename.substr(dot + 1);
	for (char& c : extension) {
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	}
	return extension;
}


bool
InList(const std::string& word, const char* const* list)
{
	for (; *list != nullptr; list++) {
		if (word == *list)
			return true;
	}
	return false;
}

}	// namespace


const char*
PtpStatusText(PtpStatus status)
{
	switch (status) {
		case kPtpOk:
			return "No error";
		case kPtpIoError:
			return "The connection to the device failed";
		case kPtpTimeout:
			return "The device did not answer";
		case kPtpNoDevice:
			return "The device was disconnected";
		case kPtpProtocol:
			return "The device sent an answer that could not be understood";
		case kPtpCancelled:
			return "Cancelled";
		case kPtpAccessDenied:
			return "The device does not allow access to its pictures. Unlock "
				"it and allow access when it asks";
		case kPtpBusy:
			return "The device is busy";
		case kPtpNotSupported:
			return "The device does not support this";
		case kPtpStoreUnavailable:
			return "The device has no memory card or its storage is not "
				"available";
		case kPtpInvalidObject:
			return "The picture is not on the device any more";
		case kPtpReadOnly:
			return "The device or the picture is write-protected";
		case kPtpNoSession:
			return "The device is not open";
		case kPtpFailed:
			return "The device reported an error";
	}
	return "Unknown error";
}


std::string
PtpResponseName(uint16_t code)
{
	static const struct {
		uint16_t code;
		const char* name;
	} kNames[] = {
		{0x2000, "Undefined"}, {0x2001, "OK"}, {0x2002, "General_Error"},
		{0x2003, "Session_Not_Open"}, {0x2004, "Invalid_TransactionID"},
		{0x2005, "Operation_Not_Supported"},
		{0x2006, "Parameter_Not_Supported"}, {0x2007, "Incomplete_Transfer"},
		{0x2008, "Invalid_StorageID"}, {0x2009, "Invalid_ObjectHandle"},
		{0x200A, "DeviceProp_Not_Supported"},
		{0x200B, "Invalid_ObjectFormatCode"}, {0x200C, "Store_Full"},
		{0x200D, "Object_WriteProtected"}, {0x200E, "Store_Read-Only"},
		{0x200F, "Access_Denied"}, {0x2010, "No_Thumbnail_Present"},
		{0x2011, "SelfTest_Failed"}, {0x2012, "Partial_Deletion"},
		{0x2013, "Store_Not_Available"},
		{0x2014, "Specification_By_Format_Unsupported"},
		{0x2015, "No_Valid_ObjectInfo"}, {0x2016, "Invalid_Code_Format"},
		{0x2017, "Unknown_Vendor_Code"}, {0x2018, "Capture_Already_Terminated"},
		{0x2019, "Device_Busy"}, {0x201A, "Invalid_ParentObject"},
		{0x201B, "Invalid_DeviceProp_Format"},
		{0x201C, "Invalid_DeviceProp_Value"}, {0x201D, "Invalid_Parameter"},
		{0x201E, "Session_Already_Open"}, {0x201F, "Transaction_Cancelled"},
		{0x2020, "Specification_of_Destination_Unsupported"},
		{0xA801, "Invalid_ObjectPropCode"},
		{0xA802, "Invalid_ObjectProp_Format"},
		{0xA803, "Invalid_ObjectProp_Value"},
		{0xA804, "Invalid_ObjectReference"}, {0xA805, "Group_Not_Supported"},
		{0xA806, "Invalid_Dataset"},
		{0xA807, "Specification_By_Group_Unsupported"},
		{0xA808, "Specification_By_Depth_Unsupported"},
		{0xA809, "Object_Too_Large"}, {0xA80A, "ObjectProp_Not_Supported"}
	};
	for (const auto& entry : kNames) {
		if (entry.code == code)
			return entry.name;
	}
	char text[16];
	snprintf(text, sizeof(text), "0x%04x", code);
	return text;
}


bool
PtpDeviceInfo::Supports(uint16_t operation) const
{
	return std::find(operations.begin(), operations.end(), operation)
		!= operations.end();
}


bool
PtpDeviceInfo::IsMtp() const
{
	// MTP's vendor extension is Microsoft's (6), but phones say other
	// things too: the object property operations are what tells.
	return vendorExtensionId == 6 || Supports(kMtpOpGetObjectPropsSupported)
		|| Supports(kMtpOpGetObjectPropList);
}


int64_t
PtpParseDate(const std::string& text, bool* isUtc)
{
	if (isUtc != nullptr)
		*isUtc = false;
	int year, month, day, hour, minute, second;
	if (text.size() < 15 || text[8] != 'T' || !Digits(text, 0, 4, year)
		|| !Digits(text, 4, 2, month) || !Digits(text, 6, 2, day)
		|| !Digits(text, 9, 2, hour) || !Digits(text, 11, 2, minute)
		|| !Digits(text, 13, 2, second)) {
		return 0;
	}
	if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31
		|| hour > 23 || minute > 59 || second > 60) {
		return 0;
	}
	int64_t seconds = DaysFromCivil(year, (unsigned)month, (unsigned)day)
			* 86400
		+ hour * 3600 + minute * 60 + second;
	size_t at = 15;
	if (at < text.size() && text[at] == '.') {
		at++;
		while (at < text.size() && text[at] >= '0' && text[at] <= '9')
			at++;
	}
	if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) {
		if (isUtc != nullptr)
			*isUtc = true;
	} else if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
		int zoneHours, zoneMinutes;
		if (Digits(text, at + 1, 2, zoneHours)
			&& Digits(text, at + 3, 2, zoneMinutes) && zoneHours < 24
			&& zoneMinutes < 60) {
			int offset = zoneHours * 3600 + zoneMinutes * 60;
			seconds -= text[at] == '+' ? offset : -offset;
			if (isUtc != nullptr)
				*isUtc = true;
		}
	}
	return seconds > 0 ? seconds : 0;
}


PtpMediaKind
PtpMediaKindOf(const std::string& filename, uint16_t format)
{
	static const char* const kPictures[] = {"jpg", "jpeg", "jpe", "png",
		"heic", "heif", "hif", "avif", "webp", "tif", "tiff", "gif", "bmp",
		"jp2", "jxl", nullptr};
	static const char* const kRaw[] = {"dng", "cr2", "cr3", "crw", "nef",
		"nrw", "arw", "srf", "sr2", "raf", "rw2", "rwl", "orf", "pef", "srw",
		"x3f", "3fr", "fff", "erf", "kdc", "dcr", "mrw", "iiq", "mos", "raw",
		"gpr", nullptr};
	static const char* const kVideos[] = {"mov", "mp4", "m4v", "avi", "mts",
		"m2ts", "3gp", "3g2", "mkv", "mpg", "mpeg", "wmv", "webm", "insv",
		nullptr};
	std::string extension = LowerExtension(filename);
	if (!extension.empty()) {
		if (InList(extension, kPictures))
			return kPtpMediaPicture;
		if (InList(extension, kRaw))
			return kPtpMediaRaw;
		if (InList(extension, kVideos))
			return kPtpMediaVideo;
		// A name with an extension we do not know is believed over the
		// format code only for the things cameras store beside pictures.
		static const char* const kOther[] = {"thm", "xmp", "lrv", "aae",
			"wav", "mp3", "txt", "ctg", "dat", "bin", "plist", "db", nullptr};
		if (InList(extension, kOther))
			return kPtpMediaOther;
	}
	switch (format) {
		case kPtpFormatJpeg:
		case kPtpFormatJfif:
		case kPtpFormatPng:
		case kPtpFormatTiff:
		case kPtpFormatBmp:
		case kPtpFormatGif:
		case kPtpFormatJp2:
		case kPtpFormatHeif:
			return kPtpMediaPicture;
		case kPtpFormatDng:
		case kPtpFormatTiffEp:
			return kPtpMediaRaw;
		case kPtpFormatAvi:
		case kPtpFormatMpeg:
		case kPtpFormatAsf:
		case kPtpFormatQuickTime:
		case kMtpFormatWmv:
		case kMtpFormatMp4:
		case kMtpFormatMp2:
		case kMtpFormat3gp:
			return kPtpMediaVideo;
	}
	return kPtpMediaOther;
}


bool
ExifThumbnail(const uint8_t* data, size_t length, std::vector<uint8_t>& jpeg)
{
	jpeg.clear();
	if (length < 4 || data[0] != 0xFF || data[1] != 0xD8)
		return false;
	size_t at = 2;
	while (at + 4 <= length) {
		if (data[at] != 0xFF)
			return false;
		uint8_t marker = data[at + 1];
		if (marker == 0xFF) {
			at++;
			continue;
		}
		if (marker == 0xDA || marker == 0xD9)
			return false;
		size_t segment = ((size_t)data[at + 2] << 8) | data[at + 3];
		if (segment < 2)
			return false;
		// A segment that begins in the buffer may end beyond it: the
		// thumbnail has to be inside.
		size_t end = std::min(length, at + 2 + segment);
		if (marker == 0xE1 && end - at >= 10 + 8
			&& memcmp(data + at + 4, "Exif\0\0", 6) == 0) {
			const uint8_t* tiff = data + at + 10;
			size_t size = end - (at + 10);
			bool big;
			if (tiff[0] == 'I' && tiff[1] == 'I')
				big = false;
			else if (tiff[0] == 'M' && tiff[1] == 'M')
				big = true;
			else
				return false;
			auto get16 = [&](size_t offset) -> uint32_t {
				return big ? (uint32_t)((tiff[offset] << 8) | tiff[offset + 1])
					: (uint32_t)(tiff[offset] | (tiff[offset + 1] << 8));
			};
			auto get32 = [&](size_t offset) -> uint32_t {
				return big
					? ((uint32_t)tiff[offset] << 24)
						| ((uint32_t)tiff[offset + 1] << 16)
						| ((uint32_t)tiff[offset + 2] << 8) | tiff[offset + 3]
					: (uint32_t)tiff[offset] | ((uint32_t)tiff[offset + 1] << 8)
						| ((uint32_t)tiff[offset + 2] << 16)
						| ((uint32_t)tiff[offset + 3] << 24);
			};
			if (get16(2) != 42)
				return false;
			size_t ifd = get32(4);
			if (ifd > size || size - ifd < 2)
				return false;
			size_t count = get16(ifd);
			size_t next = ifd + 2 + count * 12;
			if (next > size || size - next < 4)
				return false;
			ifd = get32(next);
			if (ifd == 0 || ifd > size || size - ifd < 2)
				return false;
			count = get16(ifd);
			if (ifd + 2 + count * 12 > size)
				return false;
			uint32_t offset = 0, bytes = 0;
			for (size_t i = 0; i < count; i++) {
				size_t entry = ifd + 2 + i * 12;
				uint32_t tag = get16(entry);
				uint32_t type = get16(entry + 2);
				uint32_t value = type == 3 ? get16(entry + 8) : get32(entry + 8);
				if (tag == 0x0201)
					offset = value;
				else if (tag == 0x0202)
					bytes = value;
			}
			if (offset == 0 || bytes < 4 || offset > size
				|| bytes > size - offset) {
				return false;
			}
			if (tiff[offset] != 0xFF || tiff[offset + 1] != 0xD8)
				return false;
			jpeg.assign(tiff + offset, tiff + offset + bytes);
			return true;
		}
		at += 2 + segment;
	}
	return false;
}


// #pragma mark - PtpSession


struct PtpSession::Result {
	uint16_t	code = 0;
	uint32_t	params[5] = {0, 0, 0, 0, 0};
	int			paramCount = 0;
};


PtpSession::PtpSession(PtpTransport& transport)
	:
	fTransport(transport),
	fOpen(false),
	fSessionId(1),
	fTransactionId(0),
	fLastResponse(0),
	fLastOperation(0),
	fTimeoutMs(20000)
{
}


PtpSession::~PtpSession()
{
	if (fOpen)
		Close();
}


std::string
PtpSession::LastError() const
{
	static const struct {
		uint16_t code;
		const char* name;
	} kOperations[] = {
		{kPtpOpGetDeviceInfo, "GetDeviceInfo"},
		{kPtpOpOpenSession, "OpenSession"},
		{kPtpOpCloseSession, "CloseSession"},
		{kPtpOpGetStorageIDs, "GetStorageIDs"},
		{kPtpOpGetStorageInfo, "GetStorageInfo"},
		{kPtpOpGetObjectHandles, "GetObjectHandles"},
		{kPtpOpGetObjectInfo, "GetObjectInfo"},
		{kPtpOpGetObject, "GetObject"}, {kPtpOpGetThumb, "GetThumb"},
		{kPtpOpDeleteObject, "DeleteObject"},
		{kPtpOpGetPartialObject, "GetPartialObject"},
		{kMtpOpGetObjectPropValue, "GetObjectPropValue"},
		{kMtpOpGetObjectPropList, "GetObjectPropList"}
	};
	std::string text;
	for (const auto& entry : kOperations) {
		if (entry.code == fLastOperation)
			text = entry.name;
	}
	if (text.empty()) {
		char code[16];
		snprintf(code, sizeof(code), "0x%04x", fLastOperation);
		text = code;
	}
	text += ": ";
	text += fLastResponse != 0 ? PtpResponseName(fLastResponse)
		: std::string("no response");
	return text;
}


PtpStatus
PtpSession::_StatusOf(uint16_t response) const
{
	switch (response) {
		case kPtpRespOk:
			return kPtpOk;
		case kPtpRespSessionNotOpen:
			return kPtpNoSession;
		case kPtpRespOperationNotSupported:
		case kPtpRespParameterNotSupported:
		case kPtpRespSpecByFormatUnsupported:
		case kPtpRespNoThumbnailPresent:
		case 0xA805:	// Group_Not_Supported
		case 0xA807:	// Specification_By_Group_Unsupported
		case 0xA808:	// Specification_By_Depth_Unsupported
		case 0xA80A:	// ObjectProp_Not_Supported
			return kPtpNotSupported;
		case kPtpRespInvalidStorageId:
		case kPtpRespStoreNotAvailable:
			return kPtpStoreUnavailable;
		case kPtpRespInvalidObjectHandle:
			return kPtpInvalidObject;
		case kPtpRespObjectWriteProtected:
		case kPtpRespStoreReadOnly:
			return kPtpReadOnly;
		case kPtpRespAccessDenied:
			return kPtpAccessDenied;
		case kPtpRespDeviceBusy:
			return kPtpBusy;
		case kPtpRespTransactionCancelled:
			return kPtpCancelled;
	}
	return kPtpFailed;
}


// One operation: command, the data the device sends (into the sink, or
// thrown away), response. A busy device is asked again a few times.
PtpStatus
PtpSession::_Transaction(uint16_t operation, const uint32_t* params,
	int paramCount, const PtpDataSink* sink, Result& result, uint64_t sizeHint,
	int timeoutFactor)
{
	if (fBuffer.empty()) {
		size_t packet = fTransport.MaxPacketIn();
		if (packet == 0 || packet > kChunkSize)
			packet = 512;
		fBuffer.resize(kChunkSize / packet * packet);
	}

	PtpStatus status = kPtpOk;
	for (int attempt = 0; attempt < 6; attempt++) {
		fLastOperation = operation;
		fLastResponse = 0;
		result = Result();

		// Outside a session (GetDeviceInfo, OpenSession) the transaction
		// number is 0; inside one they count up from 1.
		uint32_t transactionId = 0;
		if (fOpen) {
			transactionId = fTransactionId++;
			if (fTransactionId == 0xFFFFFFFF)
				fTransactionId = 1;
		}

		uint8_t command[kHeaderSize + 5 * 4];
		if (paramCount > 5)
			paramCount = 5;
		size_t length = kHeaderSize + (size_t)paramCount * 4;
		Put32(command, (uint32_t)length);
		Put16(command + 4, kTypeCommand);
		Put16(command + 6, operation);
		Put32(command + 8, transactionId);
		for (int i = 0; i < paramCount; i++)
			Put32(command + kHeaderSize + i * 4, params[i]);

		status = fTransport.BulkOut(command, length, fTimeoutMs);
		if (status != kPtpOk) {
			if (status == kPtpNoDevice)
				fOpen = false;
			else
				fTransport.ClearStalls();
			return status;
		}

		bool delivered = false;
		PtpDataSink counting;
		if (sink != nullptr) {
			counting = [&](const uint8_t* data, size_t size, uint64_t total) {
				delivered = true;
				return (*sink)(data, size, total);
			};
		}
		status = _ReadDataAndResponse(operation, transactionId,
			sink != nullptr ? &counting : nullptr, result, sizeHint,
			fTimeoutMs * timeoutFactor);
		if (status != kPtpBusy || delivered || attempt == 5)
			break;
		std::this_thread::sleep_for(
			std::chrono::milliseconds(150 * (attempt + 1)));
	}
	return status;
}


PtpStatus
PtpSession::_ReadDataAndResponse(uint16_t operation, uint32_t transactionId,
	const PtpDataSink* sink, Result& result, uint64_t sizeHint, int timeoutMs)
{
	size_t packet = fTransport.MaxPacketIn();
	if (packet < kHeaderSize || packet > fBuffer.size())
		packet = std::min<size_t>(512, fBuffer.size());

	// The first packet: the header of the data phase with the beginning of
	// the data (or alone), or the response at once. What belongs to a
	// transaction given up earlier is stepped over.
	size_t received = 0;
	PtpStatus status = kPtpOk;
	uint32_t length = 0;
	for (int skipped = 0; ; skipped++) {
		for (int empty = 0; ; empty++) {
			status = fTransport.BulkIn(fBuffer.data(), packet, &received,
				timeoutMs);
			if (status != kPtpOk || received > 0)
				break;
			// A zero-length packet that ended the transfer before this one.
			if (empty == 3) {
				status = kPtpProtocol;
				break;
			}
		}
		if (status == kPtpOk && (received < kHeaderSize || received > packet))
			status = kPtpProtocol;
		if (status != kPtpOk) {
			if (status == kPtpNoDevice) {
				fOpen = false;
				return status;
			}
			if (status == kPtpIoError)
				fTransport.ClearStalls();
			_Recover(transactionId, true);
			return status;
		}

		length = Get32(fBuffer.data());
		uint16_t type = Get16(fBuffer.data() + 4);
		uint16_t code = Get16(fBuffer.data() + 6);
		uint32_t id = Get32(fBuffer.data() + 8);
		bool stale = transactionId != 0 && id != 0 && id != transactionId;

		if (type == kTypeResponse) {
			if (stale && skipped < 4 && length == received)
				continue;
			return _ReadResponse(fBuffer.data(), received, transactionId,
				result, timeoutMs);
		}
		if (type != kTypeData
			|| (length != kUnknownLength && length < kHeaderSize)
			|| (!stale && code != operation) || (stale && skipped >= 4)) {
			_Recover(transactionId, true);
			return kPtpProtocol;
		}
		if (!stale)
			break;
		status = _SkipData(length, received, packet);
		if (status != kPtpOk) {
			if (status == kPtpNoDevice) {
				fOpen = false;
				return status;
			}
			_Recover(transactionId, true);
			return status;
		}
	}

	// How much is coming: what the header says; for objects of 4 GiB and
	// more it says "unknown", and the listing's size or the first packet
	// shorter than a full one tells.
	bool known = length != kUnknownLength;
	uint64_t total = known ? length - kHeaderSize : sizeHint;
	if (!known && sizeHint != kPtpSizeUnknown && sizeHint >= received - kHeaderSize)
		known = true;
	uint64_t remaining = known ? total : 0;

	bool complete = true;
	bool cancelled = false;
	std::vector<uint8_t> trailing;	// a response that came with the data
	bool ended = false;

	auto deliver = [&](const uint8_t* data, size_t size) {
		if (known) {
			if (size > remaining) {
				trailing.assign(data + remaining, data + size);
				size = (size_t)remaining;
			}
			remaining -= size;
		}
		if (size > 0 && sink != nullptr && !(*sink)(data, size, total))
			cancelled = true;
	};

	deliver(fBuffer.data() + kHeaderSize, received - kHeaderSize);
	// A data phase that is over with a short first packet.
	if (!known && received < packet && received > kHeaderSize)
		ended = true;

	while (!cancelled && trailing.empty()
		&& (known ? remaining > 0 : !ended)) {
		size_t want = fBuffer.size();
		if (known && remaining < want) {
			want = (size_t)remaining;
			if (want % packet != 0)
				want = (want / packet + 1) * packet;
		}
		size_t got = 0;
		status = fTransport.BulkIn(fBuffer.data(), want, &got, timeoutMs);
		if (status != kPtpOk || got > want) {
			if (status == kPtpOk)
				status = kPtpProtocol;
			if (status == kPtpNoDevice) {
				fOpen = false;
				return status;
			}
			if (status == kPtpIoError)
				fTransport.ClearStalls();
			_Recover(transactionId, true);
			return status;
		}
		if (known) {
			if (got == 0 || (got < want && got < remaining)) {
				// The device ended the data early: its response says why.
				deliver(fBuffer.data(), got);
				complete = false;
				break;
			}
			deliver(fBuffer.data(), got);
		} else {
			deliver(fBuffer.data(), got);
			if (got < want)
				ended = true;
		}
	}

	if (cancelled) {
		_Recover(transactionId, true);
		return kPtpCancelled;
	}

	status = _ReadResponse(trailing.empty() ? nullptr : trailing.data(),
		trailing.size(), transactionId, result, timeoutMs);
	if (status == kPtpOk && !complete)
		status = kPtpProtocol;
	return status;
}


// Throws away the rest of a data phase that is not this transaction's.
PtpStatus
PtpSession::_SkipData(uint32_t length, size_t received, size_t packet)
{
	if (length == kUnknownLength) {
		if (received < packet)
			return kPtpOk;
		for (size_t skipped = 0; skipped <= kMaxDrainBytes;) {
			size_t got = 0;
			PtpStatus status = fTransport.BulkIn(fBuffer.data(),
				fBuffer.size(), &got, kDrainTimeoutMs);
			if (status != kPtpOk)
				return status;
			if (got < fBuffer.size())
				return kPtpOk;
			skipped += got;
		}
		return kPtpProtocol;
	}
	uint64_t remaining = length - kHeaderSize;
	remaining -= std::min<uint64_t>(remaining, received - kHeaderSize);
	if (remaining > kMaxDrainBytes)
		return kPtpProtocol;
	while (remaining > 0) {
		size_t want = fBuffer.size();
		if (remaining < want) {
			want = (size_t)remaining;
			if (want % packet != 0)
				want = (want / packet + 1) * packet;
		}
		size_t got = 0;
		PtpStatus status = fTransport.BulkIn(fBuffer.data(), want, &got,
			kDrainTimeoutMs);
		if (status != kPtpOk)
			return status;
		if (got == 0 || (got < want && got < remaining))
			return kPtpOk;
		remaining -= std::min<uint64_t>(remaining, got);
	}
	return kPtpOk;
}


PtpStatus
PtpSession::_ReadResponse(const uint8_t* first, size_t firstLength,
	uint32_t transactionId, Result& result, int timeoutMs)
{
	uint8_t local[kHeaderSize + 5 * 4];
	for (int attempt = 0; attempt < 4; attempt++) {
		const uint8_t* data = first;
		size_t received = firstLength;
		if (data == nullptr) {
			size_t packet = fTransport.MaxPacketIn();
			if (packet < sizeof(local) || packet > fBuffer.size())
				packet = std::min<size_t>(512, fBuffer.size());
			PtpStatus status = fTransport.BulkIn(fBuffer.data(), packet,
				&received, timeoutMs);
			if (status != kPtpOk) {
				if (status == kPtpNoDevice) {
					fOpen = false;
					return status;
				}
				if (status == kPtpIoError)
					fTransport.ClearStalls();
				_Recover(transactionId, false);
				return status;
			}
			// The zero-length packet that ends a data phase of a whole
			// number of packets.
			if (received == 0)
				continue;
			if (received > packet)
				return kPtpProtocol;
			data = fBuffer.data();
		}
		first = nullptr;

		if (received < kHeaderSize || received > sizeof(local)) {
			_Recover(transactionId, false);
			return kPtpProtocol;
		}
		memcpy(local, data, received);
		uint32_t length = Get32(local);
		if (Get16(local + 4) != kTypeResponse || length < kHeaderSize
			|| length > received) {
			_Recover(transactionId, false);
			return kPtpProtocol;
		}
		// The response of a transaction given up earlier: the next one is
		// ours.
		uint32_t id = Get32(local + 8);
		if (id != transactionId && transactionId != 0 && id != 0)
			continue;
		result.code = Get16(local + 6);
		result.paramCount = (int)((length - kHeaderSize) / 4);
		if (result.paramCount > 5)
			result.paramCount = 5;
		for (int i = 0; i < result.paramCount; i++)
			result.params[i] = Get32(local + kHeaderSize + i * 4);
		fLastResponse = result.code;
		return _StatusOf(result.code);
	}
	_Recover(transactionId, false);
	return kPtpProtocol;
}


// Throws away whatever the device still sends.
PtpStatus
PtpSession::_Drain()
{
	size_t drained = 0;
	while (drained <= kMaxDrainBytes) {
		size_t got = 0;
		PtpStatus status = fTransport.BulkIn(fBuffer.data(), fBuffer.size(),
			&got, kDrainTimeoutMs);
		if (status == kPtpTimeout)
			return kPtpOk;
		if (status != kPtpOk)
			return status;
		drained += got + 1;
		// The device's last word on the transaction.
		if (got >= kHeaderSize && got <= kHeaderSize + 5 * 4
			&& Get16(fBuffer.data() + 4) == kTypeResponse
			&& Get32(fBuffer.data()) == got) {
			return kPtpOk;
		}
	}
	return kPtpBusy;
}


// After a transaction went wrong or was given up: get the device to where
// it takes a command again. Cancel request, throw away what it still
// sends, and if that does not end, reset it and open the session anew.
PtpStatus
PtpSession::_Recover(uint32_t transactionId, bool dataPending)
{
	if (fBuffer.empty())
		return kPtpOk;
	if (dataPending)
		fTransport.CancelRequest(transactionId);

	PtpStatus status = _Drain();
	if (status == kPtpNoDevice) {
		fOpen = false;
		return status;
	}
	if (status == kPtpOk) {
		for (int i = 0; i < 20; i++) {
			PtpStatus device = fTransport.DeviceStatus();
			if (device == kPtpOk || device == kPtpNotSupported)
				return kPtpOk;
			if (device == kPtpNoDevice) {
				fOpen = false;
				return device;
			}
			if (device != kPtpBusy)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	if (fTransport.ResetDevice() == kPtpNotSupported)
		return status;
	fTransport.ClearStalls();
	bool wasOpen = fOpen;
	fOpen = false;
	if (!wasOpen)
		return kPtpOk;
	// A reset closes the session.
	Result result;
	uint32_t session = fSessionId;
	status = _Transaction(kPtpOpOpenSession, &session, 1, nullptr, result);
	if (status == kPtpOk || fLastResponse == kPtpRespSessionAlreadyOpen) {
		fOpen = true;
		fTransactionId = 1;
		return kPtpOk;
	}
	return status;
}


// An operation whose data, if any, is small enough to keep.
PtpStatus
PtpSession::_Simple(uint16_t operation, const uint32_t* params, int paramCount,
	std::vector<uint8_t>* data, Result& result, size_t dataLimit,
	int timeoutFactor)
{
	if (data == nullptr)
		return _Transaction(operation, params, paramCount, nullptr, result,
			kPtpSizeUnknown, timeoutFactor);
	data->clear();
	bool tooLarge = false;
	PtpDataSink sink = [&](const uint8_t* bytes, size_t size, uint64_t) {
		if (data->size() + size > dataLimit) {
			tooLarge = true;
			return false;
		}
		data->insert(data->end(), bytes, bytes + size);
		return true;
	};
	PtpStatus status = _Transaction(operation, params, paramCount, &sink,
		result, kPtpSizeUnknown, timeoutFactor);
	if (tooLarge)
		return kPtpProtocol;
	return status;
}


PtpStatus
PtpSession::Open()
{
	fOpen = false;
	fTransactionId = 0;

	Result result;
	std::vector<uint8_t> data;
	PtpStatus status = kPtpOk;
	// A device left in the middle of something by another program gets a
	// second try after _Recover() has cleaned up.
	for (int attempt = 0; attempt < 2; attempt++) {
		status = _Simple(kPtpOpGetDeviceInfo, nullptr, 0, &data, result,
			kSmallDataLimit);
		if (status == kPtpOk || status == kPtpNoDevice
			|| status == kPtpAccessDenied) {
			break;
		}
	}
	if (status != kPtpOk)
		return status;
	if (!ParseDeviceInfo(data, fDeviceInfo))
		return kPtpProtocol;

	uint32_t session = fSessionId;
	status = _Transaction(kPtpOpOpenSession, &session, 1, nullptr, result);
	if (fLastResponse == kPtpRespSessionAlreadyOpen) {
		// Someone's session is still open: close it and open ours.
		fOpen = true;
		fTransactionId = 1;
		_Transaction(kPtpOpCloseSession, nullptr, 0, nullptr, result);
		fOpen = false;
		status = _Transaction(kPtpOpOpenSession, &session, 1, nullptr,
			result);
		if (fLastResponse == kPtpRespSessionAlreadyOpen) {
			// It keeps it: then that session is ours.
			status = kPtpOk;
		}
	}
	if (status != kPtpOk)
		return status;
	fOpen = true;
	fTransactionId = 1;
	return kPtpOk;
}


PtpStatus
PtpSession::Close()
{
	if (!fOpen)
		return kPtpOk;
	Result result;
	PtpStatus status = _Transaction(kPtpOpCloseSession, nullptr, 0, nullptr,
		result);
	fOpen = false;
	fTransactionId = 0;
	return status;
}


PtpStatus
PtpSession::GetStorageIDs(std::vector<uint32_t>& ids)
{
	ids.clear();
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	std::vector<uint8_t> data;
	PtpStatus status = _Simple(kPtpOpGetStorageIDs, nullptr, 0, &data, result,
		kSmallDataLimit);
	if (status != kPtpOk)
		return status;
	Reader reader(data.data(), data.size());
	reader.Array32(ids);
	return reader.Ok() ? kPtpOk : kPtpProtocol;
}


PtpStatus
PtpSession::GetStorageInfo(uint32_t storageId, PtpStorageInfo& info)
{
	info = PtpStorageInfo();
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	std::vector<uint8_t> data;
	PtpStatus status = _Simple(kPtpOpGetStorageInfo, &storageId, 1, &data,
		result, kSmallDataLimit);
	if (status != kPtpOk)
		return status;
	Reader reader(data.data(), data.size());
	info.storageType = reader.U16();
	info.filesystemType = reader.U16();
	info.accessCapability = reader.U16();
	info.maxCapacity = reader.U64();
	info.freeSpace = reader.U64();
	info.freeObjects = reader.U32();
	bool ok = reader.Ok();
	info.description = reader.String();
	info.volumeLabel = reader.String();
	return ok ? kPtpOk : kPtpProtocol;
}


PtpStatus
PtpSession::GetObjectHandles(uint32_t storageId, uint16_t format,
	uint32_t parent, std::vector<uint32_t>& handles)
{
	handles.clear();
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	std::vector<uint8_t> data;
	uint32_t params[3] = {storageId, format, parent};
	PtpStatus status = _Simple(kPtpOpGetObjectHandles, params, 3, &data,
		result, kListLimit, 5);
	if (status != kPtpOk)
		return status;
	Reader reader(data.data(), data.size());
	reader.Array32(handles);
	return reader.Ok() ? kPtpOk : kPtpProtocol;
}


PtpStatus
PtpSession::GetObjectInfo(uint32_t handle, PtpObjectInfo& info)
{
	info = PtpObjectInfo();
	info.handle = handle;
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	std::vector<uint8_t> data;
	PtpStatus status = _Simple(kPtpOpGetObjectInfo, &handle, 1, &data, result,
		kSmallDataLimit);
	if (status != kPtpOk)
		return status;
	return ParseObjectInfo(data, info) ? kPtpOk : kPtpProtocol;
}


PtpStatus
PtpSession::GetObject(uint32_t handle, const PtpDataSink& sink,
	uint64_t sizeHint)
{
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	return _Transaction(kPtpOpGetObject, &handle, 1, &sink, result, sizeHint);
}


PtpStatus
PtpSession::GetPartialObject(uint32_t handle, uint32_t offset,
	uint32_t maxBytes, std::vector<uint8_t>& data)
{
	data.clear();
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	uint32_t params[3] = {handle, offset, maxBytes};
	return _Simple(kPtpOpGetPartialObject, params, 3, &data, result,
		(size_t)maxBytes + 1);
}


PtpStatus
PtpSession::GetThumb(uint32_t handle, std::vector<uint8_t>& data)
{
	data.clear();
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	PtpStatus status = _Simple(kPtpOpGetThumb, &handle, 1, &data, result,
		kThumbLimit);
	if (status == kPtpOk && data.empty())
		return kPtpNotSupported;
	return status;
}


PtpStatus
PtpSession::DeleteObject(uint32_t handle)
{
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	uint32_t params[2] = {handle, 0};
	return _Transaction(kPtpOpDeleteObject, params, 2, nullptr, result);
}


PtpStatus
PtpSession::GetObjectSize(uint32_t handle, uint64_t& size)
{
	size = kPtpSizeUnknown;
	if (!fOpen)
		return kPtpNoSession;
	Result result;
	std::vector<uint8_t> data;
	uint32_t params[2] = {handle, kMtpPropObjectSize};
	PtpStatus status = _Simple(kMtpOpGetObjectPropValue, params, 2, &data,
		result, kSmallDataLimit);
	if (status != kPtpOk)
		return status;
	Reader reader(data.data(), data.size());
	uint64_t value = reader.U64();
	if (!reader.Ok())
		return kPtpProtocol;
	size = value;
	return kPtpOk;
}


// Everything the device knows about all of its objects in one answer.
PtpStatus
PtpSession::_ListByPropList(std::vector<PtpObjectInfo>& objects,
	const std::atomic<bool>* cancel, const std::function<void(int)>& progress)
{
	Result result;
	std::vector<uint8_t> data;
	// every object, every format, every property, no group, the whole tree
	uint32_t params[5] = {0xFFFFFFFF, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF};
	bool tooLarge = false;
	PtpDataSink sink = [&](const uint8_t* bytes, size_t size, uint64_t) {
		if (cancel != nullptr && *cancel)
			return false;
		if (data.size() + size > kListLimit) {
			tooLarge = true;
			return false;
		}
		data.insert(data.end(), bytes, bytes + size);
		return true;
	};
	PtpStatus status = _Transaction(kMtpOpGetObjectPropList, params, 5, &sink,
		result, kPtpSizeUnknown, 4);
	if (tooLarge)
		return kPtpProtocol;
	if (status != kPtpOk)
		return status;

	Reader reader(data.data(), data.size());
	uint32_t count = reader.U32();
	// An element is at least handle, property, type and one byte.
	if (!reader.Ok() || (uint64_t)count * 9 > reader.Remaining())
		return kPtpProtocol;

	std::unordered_map<uint32_t, size_t> index;
	size_t first = objects.size();
	for (uint32_t i = 0; i < count; i++) {
		uint32_t handle = reader.U32();
		uint16_t property = reader.U16();
		uint16_t type = reader.U16();
		if (!reader.Ok())
			return kPtpProtocol;

		uint64_t number = 0;
		std::string text;
		if (type == 0xFFFF) {
			text = reader.String();
		} else if ((type & 0x4000) != 0) {
			// An array: nothing a listing needs.
			static const uint8_t kSizes[] = {0, 1, 1, 2, 2, 4, 4, 8, 8, 16, 16};
			uint16_t element = type & 0x0FFF;
			if (element == 0 || element > 10)
				return kPtpProtocol;
			uint32_t elements = reader.U32();
			reader.Skip((uint64_t)elements * kSizes[element]);
		} else {
			switch (type) {
				case 1: case 2:
					number = reader.U8();
					break;
				case 3: case 4:
					number = reader.U16();
					break;
				case 5: case 6:
					number = reader.U32();
					break;
				case 7: case 8:
					number = reader.U64();
					break;
				case 9: case 10:
					number = reader.U64();
					reader.U64();
					break;
				default:
					return kPtpProtocol;
			}
		}
		if (!reader.Ok())
			return kPtpProtocol;

		auto found = index.find(handle);
		if (found == index.end()) {
			found = index.emplace(handle, objects.size()).first;
			PtpObjectInfo info;
			info.handle = handle;
			info.size = kPtpSizeUnknown;
			objects.push_back(info);
			if (progress && objects.size() % 256 == 0)
				progress((int)objects.size());
		}
		PtpObjectInfo& info = objects[found->second];
		switch (property) {
			case kMtpPropStorageId:
				info.storageId = (uint32_t)number;
				break;
			case kMtpPropObjectFormat:
				info.format = (uint16_t)number;
				break;
			case kMtpPropObjectSize:
				info.size = number;
				break;
			case kMtpPropObjectFileName:
				info.filename = text;
				break;
			case kMtpPropDateCreated:
				info.captured = PtpParseDate(text, &info.capturedIsUtc);
				break;
			case kMtpPropDateModified:
				info.modified = PtpParseDate(text, &info.modifiedIsUtc);
				break;
			case kMtpPropParentObject:
				info.parent = (uint32_t)number;
				break;
		}
	}

	// Objects without a name are of no use: the device's answer is not
	// what was asked for, and the caller lists the slow way.
	for (size_t i = first; i < objects.size(); i++) {
		if (objects[i].filename.empty()) {
			objects.resize(first);
			return kPtpNotSupported;
		}
	}
	if (progress)
		progress((int)objects.size());
	return kPtpOk;
}


// Folder by folder from the root, going only where the caller wants to go:
// on a phone that is the difference between its camera folder and every
// file of every program. kPtpNotSupported if the device cannot say what is
// in a folder.
PtpStatus
PtpSession::_ListByTree(const std::vector<uint32_t>& storages,
	std::vector<PtpObjectInfo>& objects, const std::atomic<bool>* cancel,
	const std::function<void(int)>& progress, const PtpFolderFilter& enter)
{
	std::unordered_set<uint32_t> seen;
	bool asked = false;
	for (uint32_t storage : storages) {
		std::vector<uint32_t> handles;
		PtpStatus status = GetObjectHandles(storage, 0, kPtpRootParent,
			handles);
		if (status == kPtpStoreUnavailable)
			continue;
		if (status == kPtpNotSupported || status == kPtpFailed) {
			objects.clear();
			return kPtpNotSupported;
		}
		if (status != kPtpOk)
			return status;

		std::vector<int> depths(handles.size(), 0);
		for (size_t next = 0; next < handles.size(); next++) {
			if (cancel != nullptr && *cancel)
				return kPtpCancelled;
			uint32_t handle = handles[next];
			if (!seen.insert(handle).second)
				continue;
			PtpObjectInfo info;
			status = GetObjectInfo(handle, info);
			if (status == kPtpInvalidObject || status == kPtpFailed
				|| status == kPtpProtocol) {
				// Gone meanwhile, or an object the device cannot describe.
				if (!fOpen)
					return status;
				continue;
			}
			if (status != kPtpOk)
				return status;
			if (info.storageId == 0)
				info.storageId = storage;
			objects.push_back(info);
			if (progress && objects.size() % 16 == 0)
				progress((int)objects.size());

			if (!info.IsFolder() || depths[next] >= 32
				|| (enter && !enter(info, depths[next]))) {
				continue;
			}
			std::vector<uint32_t> children;
			status = GetObjectHandles(storage, 0, handle, children);
			if (status == kPtpNotSupported || status == kPtpFailed) {
				if (!asked) {
					// It cannot do this at all.
					objects.clear();
					return kPtpNotSupported;
				}
				continue;
			}
			if (status == kPtpInvalidObject)
				continue;
			if (status != kPtpOk)
				return status;
			asked = true;
			for (uint32_t child : children) {
				if (seen.find(child) == seen.end()) {
					handles.push_back(child);
					depths.push_back(depths[next] + 1);
				}
			}
		}
	}
	if (progress)
		progress((int)objects.size());
	return kPtpOk;
}


// Everything at once, for devices that cannot be walked.
PtpStatus
PtpSession::_ListByHandles(const std::vector<uint32_t>& storages,
	std::vector<PtpObjectInfo>& objects, const std::atomic<bool>* cancel,
	const std::function<void(int)>& progress)
{
	std::unordered_set<uint32_t> seen;
	for (uint32_t storage : storages) {
		// Everything on the storage at once. Devices that do not answer
		// that are asked for the root; and since some answer it with the
		// root only, every folder nothing was listed in is asked for by
		// itself afterwards.
		std::vector<uint32_t> handles;
		PtpStatus status = GetObjectHandles(storage, 0, 0, handles);
		if (status == kPtpNotSupported || status == kPtpFailed)
			status = GetObjectHandles(storage, 0, kPtpRootParent, handles);
		if (status == kPtpStoreUnavailable)
			continue;
		if (status != kPtpOk)
			return status;

		std::vector<uint32_t> folders;
		std::unordered_set<uint32_t> filled;	// folders with something in them
		size_t next = 0;
		size_t nextFolder = 0;
		while (true) {
			for (; next < handles.size(); next++) {
				if (cancel != nullptr && *cancel)
					return kPtpCancelled;
				uint32_t handle = handles[next];
				if (!seen.insert(handle).second)
					continue;
				PtpObjectInfo info;
				status = GetObjectInfo(handle, info);
				if (status == kPtpInvalidObject || status == kPtpFailed
					|| status == kPtpProtocol) {
					// Gone meanwhile, or an object the device cannot
					// describe.
					if (!fOpen)
						return status;
					continue;
				}
				if (status != kPtpOk)
					return status;
				if (info.storageId == 0)
					info.storageId = storage;
				objects.push_back(info);
				if (info.parent != 0 && info.parent != kPtpRootParent)
					filled.insert(info.parent);
				if (info.IsFolder())
					folders.push_back(handle);
				if (progress && objects.size() % 16 == 0)
					progress((int)objects.size());
			}

			bool more = false;
			for (; nextFolder < folders.size() && !more; nextFolder++) {
				if (cancel != nullptr && *cancel)
					return kPtpCancelled;
				uint32_t folder = folders[nextFolder];
				if (filled.find(folder) != filled.end())
					continue;
				std::vector<uint32_t> children;
				status = GetObjectHandles(storage, 0, folder, children);
				if (status == kPtpOk) {
					for (uint32_t child : children) {
						if (seen.find(child) == seen.end()) {
							handles.push_back(child);
							more = true;
						}
					}
				} else if (status != kPtpInvalidObject
					&& status != kPtpNotSupported && status != kPtpFailed) {
					return status;
				}
			}
			if (!more && nextFolder >= folders.size())
				break;
		}
	}
	if (progress)
		progress((int)objects.size());
	return kPtpOk;
}


PtpStatus
PtpSession::ListObjects(std::vector<PtpObjectInfo>& objects,
	const std::atomic<bool>* cancel, const std::function<void(int)>& progress,
	const PtpFolderFilter& enter)
{
	objects.clear();
	if (!fOpen)
		return kPtpNoSession;

	std::vector<uint32_t> all;
	PtpStatus status = GetStorageIDs(all);
	if (status != kPtpOk)
		return status;
	// A storage whose lower half is 0 is a slot without a card.
	std::vector<uint32_t> storages;
	for (uint32_t id : all) {
		if ((id & 0xFFFF) != 0)
			storages.push_back(id);
	}
	if (storages.empty()) {
		// A phone shows its storage only once it is unlocked and its owner
		// has allowed access; a camera has no card.
		bool phone = fDeviceInfo.IsMtp()
			|| fDeviceInfo.manufacturer.find("Apple") != std::string::npos;
		return phone ? kPtpAccessDenied : kPtpStoreUnavailable;
	}

	status = kPtpNotSupported;
	if (fDeviceInfo.Supports(kMtpOpGetObjectPropList)) {
		status = _ListByPropList(objects, cancel, progress);
		if (status == kPtpCancelled || status == kPtpNoDevice
			|| (cancel != nullptr && *cancel)) {
			objects.clear();
			return status == kPtpNoDevice ? status : kPtpCancelled;
		}
		if (status == kPtpOk && objects.empty())
			status = kPtpNotSupported;
	}
	if (status != kPtpOk) {
		objects.clear();
		if (!fOpen) {
			// The fast way upset the device and it was reset.
			return status;
		}
		status = _ListByTree(storages, objects, cancel, progress, enter);
		if (status == kPtpNotSupported && fOpen) {
			objects.clear();
			status = _ListByHandles(storages, objects, cancel, progress);
		}
		if (status != kPtpOk) {
			objects.clear();
			return status;
		}
	}

	// Objects of 4 GiB and more: MTP has the size as a property.
	if (fDeviceInfo.Supports(kMtpOpGetObjectPropValue)) {
		for (PtpObjectInfo& info : objects) {
			if (info.size == kPtpSizeUnknown && !info.IsFolder()) {
				uint64_t size;
				if (GetObjectSize(info.handle, size) == kPtpOk)
					info.size = size;
				else if (!fOpen)
					break;
			}
		}
	}
	return kPtpOk;
}

}	// namespace photos
