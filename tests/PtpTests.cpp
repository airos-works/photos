// The PTP/MTP initiator against a device made up here: a transport that
// behaves like the bulk pipes of a camera (packets, short packets,
// zero-length packets) with a small object store behind it.
#include "Test.h"

#include "ptp/Ptp.h"

#include <cstring>
#include <deque>
#include <random>

using namespace photos;

namespace {

void
W16(std::vector<uint8_t>& out, uint16_t value)
{
	out.push_back((uint8_t)value);
	out.push_back((uint8_t)(value >> 8));
}


void
W32(std::vector<uint8_t>& out, uint32_t value)
{
	W16(out, (uint16_t)value);
	W16(out, (uint16_t)(value >> 16));
}


void
W64(std::vector<uint8_t>& out, uint64_t value)
{
	W32(out, (uint32_t)value);
	W32(out, (uint32_t)(value >> 32));
}


// A PTP string from UTF-8 (the code points of the Basic Multilingual Plane
// and beyond).
void
WString(std::vector<uint8_t>& out, const std::string& text)
{
	if (text.empty()) {
		out.push_back(0);
		return;
	}
	std::vector<uint16_t> units;
	for (size_t i = 0; i < text.size();) {
		uint32_t c = (uint8_t)text[i];
		int more = 0;
		if (c >= 0xF0) {
			c &= 0x07;
			more = 3;
		} else if (c >= 0xE0) {
			c &= 0x0F;
			more = 2;
		} else if (c >= 0xC0) {
			c &= 0x1F;
			more = 1;
		}
		i++;
		for (; more > 0 && i < text.size(); more--, i++)
			c = (c << 6) | ((uint8_t)text[i] & 0x3F);
		if (c >= 0x10000) {
			c -= 0x10000;
			units.push_back((uint16_t)(0xD800 + (c >> 10)));
			units.push_back((uint16_t)(0xDC00 + (c & 0x3FF)));
		} else
			units.push_back((uint16_t)c);
	}
	units.push_back(0);
	out.push_back((uint8_t)units.size());
	for (uint16_t unit : units)
		W16(out, unit);
}


void
WArray16(std::vector<uint8_t>& out, const std::vector<uint16_t>& values)
{
	W32(out, (uint32_t)values.size());
	for (uint16_t value : values)
		W16(out, value);
}


uint32_t
R32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}


struct FakeObject {
	uint32_t				handle;
	uint32_t				parent;		// 0: root
	uint16_t				format;
	std::string				name;
	std::vector<uint8_t>	data;
	std::string				captured;
	std::string				modified;
	std::vector<uint8_t>	thumb;
};


std::vector<uint8_t>
Pattern(size_t size, uint32_t seed)
{
	std::vector<uint8_t> data(size);
	uint32_t x = seed * 2654435761u + 1;
	for (size_t i = 0; i < size; i++) {
		x = x * 1664525u + 1013904223u;
		data[i] = (uint8_t)(x >> 24);
	}
	return data;
}


class FakeDevice : public PtpTransport {
public:
	// How the device behaves.
	size_t		packet = 512;
	bool		splitHeader = false;	// the data header as a packet of its own
	bool		zlp = true;				// ends whole-packet transfers properly
	bool		unknownLength = false;	// data containers say 0xFFFFFFFF
	bool		propList = false;		// MTP GetObjectPropList
	bool		mtp = false;
	bool		locked = false;			// no storage
	bool		sessionOpen = false;	// someone's session is open
	bool		stubbornSession = false;
	int			busy = 0;				// answers Device_Busy this often
	bool		cancelSupported = true;
	bool		cancelResponds = false;	// Transaction_Cancelled after a cancel
	bool		allHandles = true;		// GetObjectHandles for a whole storage
	bool		rootOnly = false;		// ... answered with the root's contents
	bool		noParentFilter = false;	// cannot list one folder
	bool		readOnly = false;
	long		truncate = -1;			// sends only this much of an object
	bool		silent = false;			// never answers
	int			garbage = 0;			// answers with noise this often
	bool		staleResponse = false;	// an old response before the real one
	std::vector<FakeObject> objects;

	// What happened.
	int			commands = 0;
	int			cancels = 0;
	int			resets = 0;
	bool		overflow = false;		// the host asked for less than a packet
	bool		session = false;
	std::vector<uint16_t> log;

	FakeDevice()
	{
		Add(0, kPtpFormatAssociation, "DCIM", 0);							// 1
		Add(1, kPtpFormatAssociation, "100TEST", 0);						// 2
		Add(2, kPtpFormatJpeg, "IMG_0001.JPG", 500, "20261121T143000");		// 3
		Add(2, kPtpFormatJpeg, "IMG_0002.JPG", 70001, "20261121T143102.5");	// 4
		Add(2, kPtpFormatUndefined, "DSCF0003.RAF", 1024 * 1024 + 3,
			"20261120T090000+1100");										// 5
		Add(2, kPtpFormatQuickTime, "MVI_0004.MOV", 300000,
			"20261119T235959Z");											// 6
		Add(2, kPtpFormatJpeg, "Stra\xC3\x9F" "e \xE6\x97\xA5\xE6\x9C\xAC.jpg",
			12, "20260101T000000");											// 7
		Add(0, kPtpFormatUndefined, "notes.txt", 20);						// 8
		objects[2].thumb = Pattern(3000, 77);
	}

	void Add(uint32_t parent, uint16_t format, const std::string& name,
		size_t size, const std::string& date = "")
	{
		FakeObject object;
		object.handle = (uint32_t)objects.size() + 1;
		object.parent = parent;
		object.format = format;
		object.name = name;
		object.data = Pattern(size, object.handle);
		object.captured = date;
		object.modified = date;
		objects.push_back(object);
	}

	FakeObject* Find(uint32_t handle)
	{
		for (FakeObject& object : objects) {
			if (object.handle == handle)
				return &object;
		}
		return nullptr;
	}

	virtual size_t MaxPacketIn() const { return packet; }
	virtual size_t MaxPacketOut() const { return packet; }

	virtual PtpStatus BulkOut(const uint8_t* data, size_t length, int)
	{
		if (length < 12 || R32(data) != length || data[4] != 1)
			return kPtpIoError;
		uint16_t code = (uint16_t)(data[6] | (data[7] << 8));
		uint32_t id = R32(data + 8);
		uint32_t params[5] = {0, 0, 0, 0, 0};
		for (size_t i = 0; i < 5 && 12 + i * 4 + 4 <= length; i++)
			params[i] = R32(data + 12 + i * 4);
		commands++;
		log.push_back(code);
		_Handle(code, id, params);
		return kPtpOk;
	}

	virtual PtpStatus BulkIn(uint8_t* data, size_t capacity, size_t* received,
		int)
	{
		*received = 0;
		if (fPackets.empty())
			return kPtpTimeout;
		size_t got = 0;
		while (true) {
			if (fPackets.empty()) {
				// The transfer would go on waiting.
				return kPtpTimeout;
			}
			std::vector<uint8_t>& next = fPackets.front();
			if (got + next.size() > capacity) {
				overflow = true;
				return kPtpIoError;
			}
			if (!next.empty())
				memcpy(data + got, next.data(), next.size());
			got += next.size();
			bool isShort = next.size() < packet;
			fPackets.pop_front();
			if (isShort || got == capacity)
				break;
		}
		*received = got;
		return kPtpOk;
	}

	virtual PtpStatus CancelRequest(uint32_t id)
	{
		cancels++;
		if (!cancelSupported)
			return kPtpNotSupported;
		fPackets.clear();
		if (cancelResponds)
			_Respond(kPtpRespTransactionCancelled, id);
		return kPtpOk;
	}

	virtual PtpStatus DeviceStatus() { return kPtpOk; }

	virtual PtpStatus ResetDevice()
	{
		resets++;
		fPackets.clear();
		session = false;
		return kPtpOk;
	}

	virtual PtpStatus ClearStalls() { return kPtpOk; }

	bool Idle() const { return fPackets.empty(); }

	// For the noise tests: what the device says next, whatever is asked.
	void Push(const std::vector<uint8_t>& bytes) { _Write(bytes); }

private:
	void _Write(const std::vector<uint8_t>& bytes)
	{
		size_t at = 0;
		while (bytes.size() - at >= packet) {
			fPackets.emplace_back(bytes.begin() + at,
				bytes.begin() + at + packet);
			at += packet;
		}
		if (at < bytes.size())
			fPackets.emplace_back(bytes.begin() + at, bytes.end());
		else if (zlp && !bytes.empty())
			fPackets.emplace_back();
	}

	void _Respond(uint16_t code, uint32_t id,
		const std::vector<uint32_t>& params = {})
	{
		std::vector<uint8_t> out;
		W32(out, (uint32_t)(12 + params.size() * 4));
		W16(out, 3);
		W16(out, code);
		W32(out, id);
		for (uint32_t param : params)
			W32(out, param);
		_Write(out);
	}

	void _Data(uint16_t code, uint32_t id, const std::vector<uint8_t>& payload,
		long sendOnly = -1)
	{
		std::vector<uint8_t> out;
		W32(out, unknownLength ? 0xFFFFFFFF : (uint32_t)(12 + payload.size()));
		W16(out, 2);
		W16(out, code);
		W32(out, id);
		size_t size = sendOnly >= 0 && (size_t)sendOnly < payload.size()
			? (size_t)sendOnly : payload.size();
		if (splitHeader) {
			// 12 bytes: a short packet by itself
			_Write(out);
			if (size > 0)
				_Write(std::vector<uint8_t>(payload.begin(),
					payload.begin() + size));
		} else {
			out.insert(out.end(), payload.begin(), payload.begin() + size);
			_Write(out);
		}
	}

	std::vector<uint8_t> _ObjectInfo(const FakeObject& object)
	{
		std::vector<uint8_t> out;
		W32(out, 0x00010001);
		W16(out, object.format);
		W16(out, 0);
		W32(out, (uint32_t)object.data.size());
		W16(out, object.thumb.empty() ? 0 : kPtpFormatJpeg);
		W32(out, (uint32_t)object.thumb.size());
		W32(out, object.thumb.empty() ? 0 : 160);
		W32(out, object.thumb.empty() ? 0 : 120);
		W32(out, 4000);
		W32(out, 3000);
		W32(out, 24);
		W32(out, object.parent);
		W16(out, object.format == kPtpFormatAssociation ? 1 : 0);
		W32(out, 0);
		W32(out, 0);
		WString(out, object.name);
		WString(out, object.captured);
		WString(out, object.modified);
		WString(out, "");
		return out;
	}

	void _Handle(uint16_t code, uint32_t id, const uint32_t* params)
	{
		if (silent)
			return;
		if (garbage > 0) {
			garbage--;
			_Write(Pattern(100 + garbage * 37, (uint32_t)garbage));
			return;
		}
		if (busy > 0 && code != kPtpOpGetDeviceInfo) {
			busy--;
			_Respond(kPtpRespDeviceBusy, id);
			return;
		}
		if (staleResponse && id > 1) {
			staleResponse = false;
			_Respond(kPtpRespOk, id - 1);
		}
		if (code != kPtpOpGetDeviceInfo && code != kPtpOpOpenSession
			&& !session && !sessionOpen) {
			_Respond(kPtpRespSessionNotOpen, id);
			return;
		}
		switch (code) {
			case kPtpOpGetDeviceInfo:
			{
				std::vector<uint8_t> out;
				W16(out, 100);
				W32(out, mtp ? 6 : 0);
				W16(out, 100);
				WString(out, mtp ? "microsoft.com: 1.0;" : "");
				W16(out, 0);
				std::vector<uint16_t> operations = {0x1001, 0x1002, 0x1003,
					0x1004, 0x1005, 0x1007, 0x1008, 0x1009, 0x100A, 0x100B,
					0x101B};
				if (mtp) {
					operations.push_back(kMtpOpGetObjectPropsSupported);
					operations.push_back(kMtpOpGetObjectPropValue);
				}
				if (propList)
					operations.push_back(kMtpOpGetObjectPropList);
				WArray16(out, operations);
				WArray16(out, {});
				WArray16(out, {});
				WArray16(out, {});
				WArray16(out, {kPtpFormatJpeg});
				WString(out, "Fake Cameras");
				WString(out, "Model \xC3\x9C 1");
				WString(out, "1.0");
				WString(out, "SN0001");
				_Data(code, id, out);
				_Respond(kPtpRespOk, id);
				break;
			}
			case kPtpOpOpenSession:
				if (sessionOpen || session) {
					_Respond(kPtpRespSessionAlreadyOpen, id);
				} else {
					session = true;
					_Respond(kPtpRespOk, id);
				}
				break;
			case kPtpOpCloseSession:
				if (!stubbornSession)
					sessionOpen = false;
				session = false;
				_Respond(kPtpRespOk, id);
				break;
			case kPtpOpGetStorageIDs:
			{
				std::vector<uint8_t> out;
				W32(out, locked ? 0 : 2);
				if (!locked) {
					W32(out, 0x00010001);
					W32(out, 0x00020000);	// an empty slot
				}
				_Data(code, id, out);
				_Respond(kPtpRespOk, id);
				break;
			}
			case kPtpOpGetStorageInfo:
			{
				if (params[0] != 0x00010001) {
					_Respond(kPtpRespStoreNotAvailable, id);
					break;
				}
				std::vector<uint8_t> out;
				W16(out, 4);
				W16(out, 3);
				W16(out, readOnly ? 1 : 0);
				W64(out, 64ull << 30);
				W64(out, 48ull << 30);
				W32(out, 12345);
				WString(out, "SD card");
				WString(out, "EOS_DIGITAL");
				_Data(code, id, out);
				_Respond(kPtpRespOk, id);
				break;
			}
			case kPtpOpGetObjectHandles:
			{
				if (params[0] != 0x00010001 && params[0] != kPtpAllStorages) {
					_Respond(kPtpRespStoreNotAvailable, id);
					break;
				}
				if ((params[2] == 0 && !allHandles)
					|| (params[2] != 0 && noParentFilter)) {
					_Respond(kPtpRespParameterNotSupported, id);
					break;
				}
				std::vector<uint32_t> handles;
				for (const FakeObject& object : objects) {
					if (params[2] == 0 && rootOnly) {
						if (object.parent == 0)
							handles.push_back(object.handle);
						continue;
					}
					if (params[2] == 0
						|| (params[2] == kPtpRootParent && object.parent == 0)
						|| object.parent == params[2]) {
						if (params[2] != 0 && params[2] != kPtpRootParent
							&& object.parent == 0) {
							continue;
						}
						handles.push_back(object.handle);
					}
				}
				std::vector<uint8_t> out;
				W32(out, (uint32_t)handles.size());
				for (uint32_t handle : handles)
					W32(out, handle);
				_Data(code, id, out);
				_Respond(kPtpRespOk, id);
				break;
			}
			case kPtpOpGetObjectInfo:
			{
				FakeObject* object = Find(params[0]);
				if (object == nullptr) {
					_Respond(kPtpRespInvalidObjectHandle, id);
					break;
				}
				_Data(code, id, _ObjectInfo(*object));
				_Respond(kPtpRespOk, id);
				break;
			}
			case kPtpOpGetObject:
			{
				FakeObject* object = Find(params[0]);
				if (object == nullptr) {
					_Respond(kPtpRespInvalidObjectHandle, id);
					break;
				}
				_Data(code, id, object->data, truncate);
				_Respond(truncate >= 0 ? kPtpRespIncompleteTransfer
					: kPtpRespOk, id);
				break;
			}
			case kPtpOpGetPartialObject:
			{
				FakeObject* object = Find(params[0]);
				if (object == nullptr) {
					_Respond(kPtpRespInvalidObjectHandle, id);
					break;
				}
				size_t offset = std::min<size_t>(params[1],
					object->data.size());
				size_t size = std::min<size_t>(params[2],
					object->data.size() - offset);
				_Data(code, id, std::vector<uint8_t>(
					object->data.begin() + offset,
					object->data.begin() + offset + size));
				_Respond(kPtpRespOk, id, {(uint32_t)size});
				break;
			}
			case kPtpOpGetThumb:
			{
				FakeObject* object = Find(params[0]);
				if (object == nullptr)
					_Respond(kPtpRespInvalidObjectHandle, id);
				else if (object->thumb.empty())
					_Respond(kPtpRespNoThumbnailPresent, id);
				else {
					_Data(code, id, object->thumb);
					_Respond(kPtpRespOk, id);
				}
				break;
			}
			case kPtpOpDeleteObject:
			{
				FakeObject* object = Find(params[0]);
				if (object == nullptr)
					_Respond(kPtpRespInvalidObjectHandle, id);
				else if (readOnly)
					_Respond(kPtpRespStoreReadOnly, id);
				else {
					objects.erase(objects.begin() + (object - objects.data()));
					_Respond(kPtpRespOk, id);
				}
				break;
			}
			case kMtpOpGetObjectPropValue:
			{
				if (!mtp) {
					_Respond(kPtpRespOperationNotSupported, id);
					break;
				}
				FakeObject* object = Find(params[0]);
				if (object == nullptr || params[1] != kMtpPropObjectSize) {
					_Respond(kPtpRespInvalidObjectHandle, id);
					break;
				}
				std::vector<uint8_t> out;
				W64(out, object->data.size());
				_Data(code, id, out);
				_Respond(kPtpRespOk, id);
				break;
			}
			case kMtpOpGetObjectPropList:
			{
				if (!propList) {
					_Respond(kPtpRespOperationNotSupported, id);
					break;
				}
				std::vector<uint8_t> out;
				W32(out, (uint32_t)objects.size() * 9);
				for (const FakeObject& object : objects) {
					auto element = [&](uint16_t property, uint16_t type) {
						W32(out, object.handle);
						W16(out, property);
						W16(out, type);
					};
					element(kMtpPropStorageId, 6);
					W32(out, 0x00010001);
					element(kMtpPropObjectFormat, 4);
					W16(out, object.format);
					element(kMtpPropObjectSize, 8);
					W64(out, object.data.size());
					element(kMtpPropObjectFileName, 0xFFFF);
					WString(out, object.name);
					element(kMtpPropDateCreated, 0xFFFF);
					WString(out, object.captured);
					element(kMtpPropDateModified, 0xFFFF);
					WString(out, object.modified);
					element(kMtpPropParentObject, 6);
					W32(out, object.parent);
					// things a listing has to step over
					element(0xDC41, 10);	// a 128 bit identifier
					W64(out, object.handle);
					W64(out, 0);
					element(0xDC99, 0x4004);	// an array of uint16
					W32(out, 3);
					W16(out, 1);
					W16(out, 2);
					W16(out, 3);
				}
				_Data(code, id, out);
				_Respond(kPtpRespOk, id);
				break;
			}
			default:
				_Respond(kPtpRespOperationNotSupported, id);
				break;
		}
	}

	std::deque<std::vector<uint8_t>> fPackets;
};


struct Collector {
	std::vector<uint8_t>	data;
	uint64_t				total = 0;
	size_t					calls = 0;
	size_t					stopAfter = SIZE_MAX;

	PtpDataSink Sink()
	{
		return [this](const uint8_t* bytes, size_t size, uint64_t all) {
			calls++;
			total = all;
			data.insert(data.end(), bytes, bytes + size);
			return data.size() < stopAfter;
		};
	}
};


const PtpObjectInfo*
Named(const std::vector<PtpObjectInfo>& objects, const std::string& name)
{
	for (const PtpObjectInfo& info : objects) {
		if (info.filename == name)
			return &info;
	}
	return nullptr;
}

}	// namespace


TEST(PtpDates)
{
	bool utc = true;
	CHECK_EQ(PtpParseDate("19700101T000000", &utc), (int64_t)0);
	CHECK_EQ(PtpParseDate("19700102T000001", &utc), (int64_t)86401);
	CHECK(!utc);
	// 2026-11-21 14:30:00
	CHECK_EQ(PtpParseDate("20261121T143000"), (int64_t)1795271400);
	CHECK_EQ(PtpParseDate("20261121T143000.5"), (int64_t)1795271400);
	CHECK_EQ(PtpParseDate("20261121T143000Z", &utc), (int64_t)1795271400);
	CHECK(utc);
	CHECK_EQ(PtpParseDate("20261121T143000+1100", &utc),
		(int64_t)1795271400 - 11 * 3600);
	CHECK(utc);
	CHECK_EQ(PtpParseDate("20261121T143000.0-0530", &utc),
		(int64_t)1795271400 + 5 * 3600 + 1800);
	CHECK_EQ(PtpParseDate("20240229T120000"), (int64_t)1709208000);
	CHECK_EQ(PtpParseDate(""), (int64_t)0);
	CHECK_EQ(PtpParseDate("2026-11-21 14:30"), (int64_t)0);
	CHECK_EQ(PtpParseDate("20261321T143000"), (int64_t)0);
	CHECK_EQ(PtpParseDate("20261121T143000+"), (int64_t)1795271400);
	CHECK_EQ(PtpParseDate("2026112XT143000"), (int64_t)0);
}


TEST(PtpMediaKinds)
{
	CHECK_EQ(PtpMediaKindOf("IMG_0001.JPG"), kPtpMediaPicture);
	CHECK_EQ(PtpMediaKindOf("img.heic"), kPtpMediaPicture);
	CHECK_EQ(PtpMediaKindOf("DSCF1725.RAF"), kPtpMediaRaw);
	CHECK_EQ(PtpMediaKindOf("a.b.CR3"), kPtpMediaRaw);
	CHECK_EQ(PtpMediaKindOf("MVI_1.MOV"), kPtpMediaVideo);
	CHECK_EQ(PtpMediaKindOf("notes.txt", kPtpFormatJpeg), kPtpMediaOther);
	CHECK_EQ(PtpMediaKindOf("IMG_0001.THM", kPtpFormatJpeg), kPtpMediaOther);
	CHECK_EQ(PtpMediaKindOf("noextension", kPtpFormatJpeg), kPtpMediaPicture);
	CHECK_EQ(PtpMediaKindOf("x.unknown", kPtpFormatQuickTime), kPtpMediaVideo);
	CHECK_EQ(PtpMediaKindOf("x.", 0), kPtpMediaOther);
	CHECK_EQ(PtpMediaKindOf("", kPtpFormatDng), kPtpMediaRaw);
}


static std::vector<uint8_t>
JpegWithThumbnail(bool bigEndian, const std::vector<uint8_t>& thumb)
{
	std::vector<uint8_t> tiff;
	auto p16 = [&](uint16_t v) {
		if (bigEndian) {
			tiff.push_back((uint8_t)(v >> 8));
			tiff.push_back((uint8_t)v);
		} else {
			tiff.push_back((uint8_t)v);
			tiff.push_back((uint8_t)(v >> 8));
		}
	};
	auto p32 = [&](uint32_t v) {
		if (bigEndian) {
			p16((uint16_t)(v >> 16));
			p16((uint16_t)v);
		} else {
			p16((uint16_t)v);
			p16((uint16_t)(v >> 16));
		}
	};
	tiff.push_back(bigEndian ? 'M' : 'I');
	tiff.push_back(bigEndian ? 'M' : 'I');
	p16(42);
	p32(8);
	// IFD0: one entry (orientation), then IFD1 at 8 + 2 + 12 + 4 = 26
	p16(1);
	p16(0x0112); p16(3); p32(1); p16(1); p16(0);
	p32(26);
	// IFD1: compression, offset, length; data at 26 + 2 + 36 + 4 = 68
	p16(3);
	p16(0x0103); p16(3); p32(1); p16(6); p16(0);
	p16(0x0201); p16(4); p32(1); p32(68);
	p16(0x0202); p16(4); p32(1); p32((uint32_t)thumb.size());
	p32(0);
	tiff.insert(tiff.end(), thumb.begin(), thumb.end());

	std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 'J', 'F'};
	jpeg.push_back(0xFF);
	jpeg.push_back(0xE1);
	size_t segment = 2 + 6 + tiff.size();
	jpeg.push_back((uint8_t)(segment >> 8));
	jpeg.push_back((uint8_t)segment);
	const char exif[6] = {'E', 'x', 'i', 'f', 0, 0};
	jpeg.insert(jpeg.end(), exif, exif + 6);
	jpeg.insert(jpeg.end(), tiff.begin(), tiff.end());
	const uint8_t rest[] = {0xFF, 0xDA, 0x00, 0x02, 1, 2, 3, 0xFF, 0xD9};
	jpeg.insert(jpeg.end(), rest, rest + sizeof(rest));
	return jpeg;
}


TEST(PtpExifThumbnail)
{
	std::vector<uint8_t> thumb = {0xFF, 0xD8, 1, 2, 3, 4, 5, 0xFF, 0xD9};
	for (int big = 0; big < 2; big++) {
		std::vector<uint8_t> jpeg = JpegWithThumbnail(big != 0, thumb);
		std::vector<uint8_t> found;
		CHECK(ExifThumbnail(jpeg.data(), jpeg.size(), found));
		CHECK(found == thumb);

		// Cut anywhere, or damaged anywhere: no crash, and nothing that is
		// not the thumbnail.
		for (size_t cut = 0; cut < jpeg.size(); cut++) {
			if (ExifThumbnail(jpeg.data(), cut, found))
				CHECK(found == thumb);
		}
		std::mt19937 random(5 + big);
		for (int i = 0; i < 4000; i++) {
			std::vector<uint8_t> damaged = jpeg;
			damaged[random() % damaged.size()] = (uint8_t)random();
			damaged[random() % damaged.size()] = (uint8_t)random();
			if (ExifThumbnail(damaged.data(), damaged.size(), found))
				CHECK(found.size() >= 4 && found[0] == 0xFF);
		}
	}
	std::vector<uint8_t> found;
	CHECK(!ExifThumbnail(nullptr, 0, found));
	const uint8_t plain[] = {0xFF, 0xD8, 0xFF, 0xDA, 0, 2, 0xFF, 0xD9};
	CHECK(!ExifThumbnail(plain, sizeof(plain), found));
}


TEST(PtpOpenAndDeviceInfo)
{
	FakeDevice device;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	CHECK(session.IsOpen());
	CHECK(device.session);
	CHECK_EQ(session.DeviceInfo().manufacturer, std::string("Fake Cameras"));
	CHECK_EQ(session.DeviceInfo().model, std::string("Model \xC3\x9C 1"));
	CHECK_EQ(session.DeviceInfo().serialNumber, std::string("SN0001"));
	CHECK(session.DeviceInfo().Supports(kPtpOpGetObject));
	CHECK(!session.DeviceInfo().IsMtp());

	std::vector<uint32_t> ids;
	CHECK_EQ(session.GetStorageIDs(ids), kPtpOk);
	CHECK_EQ(ids.size(), (size_t)2);
	PtpStorageInfo storage;
	CHECK_EQ(session.GetStorageInfo(0x00010001, storage), kPtpOk);
	CHECK_EQ(storage.volumeLabel, std::string("EOS_DIGITAL"));
	CHECK_EQ(storage.maxCapacity, (uint64_t)64 << 30);
	CHECK_EQ(session.GetStorageInfo(0x00020000, storage),
		kPtpStoreUnavailable);

	CHECK_EQ(session.Close(), kPtpOk);
	CHECK(!device.session);
	CHECK(device.Idle());
	CHECK(!device.overflow);
	// closed: nothing goes to the device
	int before = device.commands;
	CHECK_EQ(session.GetStorageIDs(ids), kPtpNoSession);
	CHECK_EQ(device.commands, before);
}


TEST(PtpSessionAlreadyOpen)
{
	FakeDevice device;
	device.sessionOpen = true;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	CHECK(device.session);
	std::vector<uint32_t> ids;
	CHECK_EQ(session.GetStorageIDs(ids), kPtpOk);

	// A device that will not let go of the session: it is used as it is.
	FakeDevice stubborn;
	stubborn.sessionOpen = true;
	stubborn.stubbornSession = true;
	PtpSession second(stubborn);
	CHECK_EQ(second.Open(), kPtpOk);
	CHECK_EQ(second.GetStorageIDs(ids), kPtpOk);
	CHECK_EQ(ids.size(), (size_t)2);
}


static void
CheckListing(const std::vector<PtpObjectInfo>& objects)
{
	CHECK_EQ(objects.size(), (size_t)8);
	const PtpObjectInfo* first = Named(objects, "IMG_0001.JPG");
	CHECK(first != nullptr);
	if (first != nullptr) {
		CHECK_EQ(first->size, (uint64_t)500);
		CHECK_EQ(first->format, (uint16_t)kPtpFormatJpeg);
		CHECK_EQ(first->parent, (uint32_t)2);
		CHECK_EQ(first->captured, (int64_t)1795271400);
		CHECK(!first->capturedIsUtc);
		CHECK_EQ(first->storageId, (uint32_t)0x00010001);
	}
	const PtpObjectInfo* raw = Named(objects, "DSCF0003.RAF");
	CHECK(raw != nullptr);
	if (raw != nullptr) {
		CHECK_EQ(raw->size, (uint64_t)(1024 * 1024 + 3));
		CHECK(raw->capturedIsUtc);
	}
	const PtpObjectInfo* folder = Named(objects, "100TEST");
	CHECK(folder != nullptr && folder->IsFolder() && folder->parent == 1);
	CHECK(Named(objects, "Stra\xC3\x9F" "e \xE6\x97\xA5\xE6\x9C\xAC.jpg")
		!= nullptr);
}


TEST(PtpListByHandles)
{
	for (int variant = 0; variant < 4; variant++) {
		FakeDevice device;
		device.splitHeader = (variant & 1) != 0;
		device.packet = (variant & 2) != 0 ? 64 : 512;
		PtpSession session(device);
		CHECK_EQ(session.Open(), kPtpOk);
		std::vector<PtpObjectInfo> objects;
		int reported = 0;
		CHECK_EQ(session.ListObjects(objects, nullptr,
			[&](int found) { reported = found; }), kPtpOk);
		CheckListing(objects);
		CHECK_EQ(reported, 8);
		CHECK(device.Idle());
		CHECK(!device.overflow);
	}
}


TEST(PtpListByFolders)
{
	FakeDevice device;
	device.allHandles = false;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	std::vector<PtpObjectInfo> objects;
	CHECK_EQ(session.ListObjects(objects), kPtpOk);
	CheckListing(objects);

	// A device that answers "everything" with what is in the root (QEMU's
	// usb-mtp), and has an empty folder too.
	FakeDevice rootOnly;
	rootOnly.rootOnly = true;
	rootOnly.Add(1, kPtpFormatAssociation, "EMPTY", 0);
	PtpSession second(rootOnly);
	CHECK_EQ(second.Open(), kPtpOk);
	CHECK_EQ(second.ListObjects(objects), kPtpOk);
	CHECK_EQ(objects.size(), (size_t)9);
	CHECK(Named(objects, "IMG_0002.JPG") != nullptr);
	CHECK(Named(objects, "EMPTY") != nullptr);

	// A device that cannot list one folder: every handle, one by one.
	for (int variant = 0; variant < 2; variant++) {
		FakeDevice flat;
		flat.noParentFilter = true;
		flat.rootOnly = variant != 0;
		PtpSession third(flat);
		CHECK_EQ(third.Open(), kPtpOk);
		CHECK_EQ(third.ListObjects(objects), kPtpOk);
		// (what is below the root cannot be had from one that answers
		// "everything" with the root and cannot list a folder either)
		if (variant == 0)
			CheckListing(objects);
		else
			CHECK_EQ(objects.size(), (size_t)2);
	}
}


TEST(PtpListOnlySomeFolders)
{
	// A phone: the camera's folder, and those of a hundred programs.
	FakeDevice device;
	device.Add(0, kPtpFormatAssociation, "Android", 0);				// 9
	device.Add(9, kPtpFormatAssociation, "data", 0);				// 10
	device.Add(10, kPtpFormatJpeg, "cache.jpg", 100);				// 11
	device.Add(2, kPtpFormatAssociation, ".thumbnails", 0);			// 12
	device.Add(12, kPtpFormatJpeg, "t.jpg", 100);					// 13
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);

	std::vector<PtpObjectInfo> objects;
	CHECK_EQ(session.ListObjects(objects), kPtpOk);
	CHECK_EQ(objects.size(), (size_t)13);

	PtpFolderFilter enter = [](const PtpObjectInfo& folder, int depth) {
		if (depth == 0)
			return folder.filename == "DCIM";
		return folder.filename[0] != '.';
	};
	size_t before = device.log.size();
	CHECK_EQ(session.ListObjects(objects, nullptr, nullptr, enter), kPtpOk);
	// the root's three, 100TEST, its five pictures and the hidden folder
	CHECK_EQ(objects.size(), (size_t)10);
	CHECK(Named(objects, "IMG_0001.JPG") != nullptr);
	CHECK(Named(objects, "Android") != nullptr);
	CHECK(Named(objects, "cache.jpg") == nullptr);
	CHECK(Named(objects, "t.jpg") == nullptr);
	// and the device was not asked about the rest
	for (size_t i = before; i < device.log.size(); i++)
		CHECK(device.log[i] != kPtpOpGetObject);
	CHECK(device.Idle());

	// With the property list everything comes anyway.
	device.propList = true;
	device.mtp = true;
	PtpSession second(device);
	CHECK_EQ(second.Open(), kPtpOk);
	CHECK_EQ(second.ListObjects(objects, nullptr, nullptr, enter), kPtpOk);
	CHECK_EQ(objects.size(), (size_t)13);
}


TEST(PtpListByPropList)
{
	for (int variant = 0; variant < 2; variant++) {
		FakeDevice device;
		device.mtp = true;
		device.propList = true;
		device.splitHeader = variant != 0;
		PtpSession session(device);
		CHECK_EQ(session.Open(), kPtpOk);
		CHECK(session.DeviceInfo().IsMtp());
		std::vector<PtpObjectInfo> objects;
		CHECK_EQ(session.ListObjects(objects), kPtpOk);
		CheckListing(objects);
		// one question instead of one per object
		int infos = 0;
		for (uint16_t code : device.log)
			infos += code == kPtpOpGetObjectInfo;
		CHECK_EQ(infos, 0);
		CHECK(device.Idle());
	}
}


TEST(PtpListLockedPhone)
{
	FakeDevice phone;
	phone.mtp = true;
	phone.locked = true;
	PtpSession session(phone);
	CHECK_EQ(session.Open(), kPtpOk);
	std::vector<PtpObjectInfo> objects;
	CHECK_EQ(session.ListObjects(objects), kPtpAccessDenied);

	FakeDevice camera;
	camera.locked = true;	// no card
	PtpSession second(camera);
	CHECK_EQ(second.Open(), kPtpOk);
	CHECK_EQ(second.ListObjects(objects), kPtpStoreUnavailable);
}


TEST(PtpListCancel)
{
	FakeDevice device;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	std::atomic<bool> cancel(true);
	std::vector<PtpObjectInfo> objects;
	CHECK_EQ(session.ListObjects(objects, &cancel), kPtpCancelled);
	CHECK(objects.empty());
	cancel = false;
	CHECK_EQ(session.ListObjects(objects, &cancel), kPtpOk);
	CHECK_EQ(objects.size(), (size_t)8);
}


TEST(PtpDownloads)
{
	// Sizes around the packet boundaries: 500 + the 12 byte header is one
	// packet exactly.
	const size_t kSizes[] = {0, 1, 499, 500, 501, 512, 1012, 1024, 4096 - 12,
		70001, 256 * 1024 - 12, 256 * 1024, 256 * 1024 + 500,
		1024 * 1024 + 3};
	for (int variant = 0; variant < 16; variant++) {
		FakeDevice device;
		device.splitHeader = (variant & 1) != 0;
		device.zlp = (variant & 2) == 0;
		device.unknownLength = (variant & 4) != 0;
		device.packet = (variant & 8) != 0 ? 64 : 512;
		device.objects.clear();
		for (size_t size : kSizes)
			device.Add(0, kPtpFormatJpeg, "x.jpg", size);
		PtpSession session(device);
		CHECK_EQ(session.Open(), kPtpOk);
		for (const FakeObject& object : device.objects) {
			// Without a length in the header and without the proper end of
			// the transfer, only the size from the listing tells where an
			// object ends.
			uint64_t hint = object.data.size();
			Collector collector;
			PtpStatus status = session.GetObject(object.handle,
				collector.Sink(), hint);
			if (status != kPtpOk || collector.data != object.data) {
				std::cerr << "variant " << variant << " size "
					<< object.data.size() << ": " << PtpStatusText(status)
					<< ", " << collector.data.size() << " bytes, "
					<< session.LastError() << "\n";
			}
			CHECK_EQ(status, kPtpOk);
			CHECK(collector.data == object.data);
			if (!object.data.empty())
				CHECK_EQ(collector.total, (uint64_t)object.data.size());
			CHECK(device.Idle());
		}
		CHECK(!device.overflow);
		CHECK_EQ(device.resets, 0);
		CHECK_EQ(device.cancels, 0);
	}
}


TEST(PtpDownloadUnknownLengthNoHint)
{
	// A device that ends its transfers properly needs no hint.
	FakeDevice device;
	device.unknownLength = true;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	for (const FakeObject& object : device.objects) {
		if (object.format == kPtpFormatAssociation || object.data.empty())
			continue;
		Collector collector;
		CHECK_EQ(session.GetObject(object.handle, collector.Sink()), kPtpOk);
		CHECK(collector.data == object.data);
		CHECK_EQ(collector.total, kPtpSizeUnknown);
	}
	CHECK(!device.overflow);
}


TEST(PtpPartialAndThumb)
{
	FakeDevice device;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	std::vector<uint8_t> data;
	CHECK_EQ(session.GetPartialObject(4, 1000, 5000, data), kPtpOk);
	const FakeObject* object = device.Find(4);
	CHECK_EQ(data.size(), (size_t)5000);
	CHECK(memcmp(data.data(), object->data.data() + 1000, 5000) == 0);
	CHECK_EQ(session.GetPartialObject(4, 70000, 5000, data), kPtpOk);
	CHECK_EQ(data.size(), (size_t)1);

	CHECK_EQ(session.GetThumb(3, data), kPtpOk);
	CHECK(data == device.Find(3)->thumb);
	CHECK_EQ(session.GetThumb(4, data), kPtpNotSupported);
	CHECK_EQ(session.LastResponse(), (uint16_t)kPtpRespNoThumbnailPresent);
	CHECK_EQ(session.GetThumb(999, data), kPtpInvalidObject);
	CHECK_EQ(session.LastError(),
		std::string("GetThumb: Invalid_ObjectHandle"));
}


TEST(PtpErrors)
{
	FakeDevice device;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	Collector collector;
	CHECK_EQ(session.GetObject(12345, collector.Sink()), kPtpInvalidObject);
	CHECK(collector.data.empty());
	PtpObjectInfo info;
	CHECK_EQ(session.GetObjectInfo(12345, info), kPtpInvalidObject);

	device.readOnly = true;
	CHECK_EQ(session.DeleteObject(3), kPtpReadOnly);
	device.readOnly = false;
	CHECK_EQ(session.DeleteObject(3), kPtpOk);
	CHECK(device.Find(3) == nullptr);
	CHECK_EQ(session.DeleteObject(3), kPtpInvalidObject);

	uint64_t size;
	CHECK_EQ(session.GetObjectSize(4, size), kPtpNotSupported);
	CHECK(device.Idle());
}


TEST(PtpBusyDevice)
{
	FakeDevice device;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	device.busy = 2;
	std::vector<uint32_t> ids;
	CHECK_EQ(session.GetStorageIDs(ids), kPtpOk);
	CHECK_EQ(ids.size(), (size_t)2);
}


TEST(PtpStaleResponse)
{
	FakeDevice device;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	std::vector<uint32_t> ids;
	CHECK_EQ(session.GetStorageIDs(ids), kPtpOk);
	device.staleResponse = true;
	PtpObjectInfo info;
	CHECK_EQ(session.GetObjectInfo(3, info), kPtpOk);
	CHECK_EQ(info.filename, std::string("IMG_0001.JPG"));
	CHECK(device.Idle());
}


TEST(PtpCancelDownload)
{
	for (int variant = 0; variant < 3; variant++) {
		FakeDevice device;
		// 0: the device takes the cancel request; 1: and answers it;
		// 2: it does not know it and sends the rest, which is thrown away
		device.cancelSupported = variant != 2;
		device.cancelResponds = variant == 1;
		PtpSession session(device);
		CHECK_EQ(session.Open(), kPtpOk);
		Collector collector;
		collector.stopAfter = 300000;
		CHECK_EQ(session.GetObject(5, collector.Sink()), kPtpCancelled);
		CHECK(collector.data.size() < 1024 * 1024);
		CHECK_EQ(device.cancels, 1);
		CHECK(device.Idle());

		// and the session goes on
		Collector second;
		CHECK_EQ(session.GetObject(4, second.Sink()), kPtpOk);
		CHECK(second.data == device.Find(4)->data);
		CHECK_EQ(device.resets, 0);
		CHECK(!device.overflow);
	}
}


TEST(PtpSilentDevice)
{
	FakeDevice device;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	device.silent = true;
	std::vector<uint32_t> ids;
	CHECK_EQ(session.GetStorageIDs(ids), kPtpTimeout);
	device.silent = false;
	CHECK_EQ(session.GetStorageIDs(ids), kPtpOk);
	CHECK_EQ(ids.size(), (size_t)2);

	FakeDevice dead;
	dead.silent = true;
	PtpSession second(dead);
	CHECK_EQ(second.Open(), kPtpTimeout);
	CHECK(!second.IsOpen());
}


TEST(PtpTruncatedObject)
{
	for (long sent : {0L, 100L, 500L, 4000L, 69999L}) {
		FakeDevice device;
		PtpSession session(device);
		CHECK_EQ(session.Open(), kPtpOk);
		device.truncate = sent;
		Collector collector;
		PtpStatus status = session.GetObject(4, collector.Sink());
		CHECK(status != kPtpOk);
		CHECK(collector.data.size() <= 70001);
		device.truncate = -1;
		// whatever it took: the device is usable again
		Collector second;
		CHECK_EQ(session.GetObject(3, second.Sink()), kPtpOk);
		CHECK(second.data == device.Find(3)->data);
		CHECK(!device.overflow);
	}
}


TEST(PtpGarbage)
{
	FakeDevice device;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	device.garbage = 3;
	std::vector<uint32_t> ids;
	for (int i = 0; i < 3; i++)
		CHECK(session.GetStorageIDs(ids) != kPtpOk);
	// The session was reset and opened again on the way, or kept.
	CHECK_EQ(session.GetStorageIDs(ids), kPtpOk);
	CHECK_EQ(ids.size(), (size_t)2);
}


// Whatever a device sends, in whatever pieces: no crash, no read outside a
// buffer (the sanitizers watch), and every call comes back.
TEST(PtpNoise)
{
	std::mt19937 random(20261121);
	for (int round = 0; round < 600; round++) {
		FakeDevice device;
		device.packet = (round % 3) == 0 ? 64 : 512;
		device.mtp = (round & 1) != 0;
		device.propList = (round & 2) != 0;
		PtpSession session(device);
		session.Open();

		// Containers that look right at first: the right type and code,
		// lengths and contents by chance.
		static const uint16_t kOperations[] = {kPtpOpGetStorageIDs,
			kPtpOpGetObjectHandles, kPtpOpGetObjectInfo, kPtpOpGetObject,
			kPtpOpGetThumb, kMtpOpGetObjectPropList, kPtpOpGetStorageInfo,
			kMtpOpGetObjectPropValue};
		uint16_t operation = kOperations[random() % 8];
		for (int piece = 0, pieces = (int)(random() % 4); piece < pieces;
			piece++) {
			std::vector<uint8_t> bytes;
			size_t size = random() % 3 == 0 ? random() % 3000 : random() % 80;
			uint32_t length = (uint32_t)(12 + size);
			switch (random() % 5) {
				case 0:
					length = 0xFFFFFFFF;
					break;
				case 1:
					length = (uint32_t)random();
					break;
				case 2:
					length = (uint32_t)(random() % 24);
					break;
			}
			W32(bytes, length);
			W16(bytes, random() % 4 == 0 ? (uint16_t)random()
				: (uint16_t)(2 + random() % 2));
			W16(bytes, random() % 4 == 0 ? (uint16_t)random() : operation);
			W32(bytes, random() % 2 == 0 ? (uint32_t)random() : 1);
			for (size_t i = 0; i < size; i++) {
				// counts that promise more than there is
				bytes.push_back(random() % 5 == 0 ? 0xFF : (uint8_t)random());
			}
			device.Push(bytes);
		}

		std::vector<uint32_t> list;
		std::vector<uint8_t> data;
		std::vector<PtpObjectInfo> objects;
		PtpObjectInfo info;
		PtpStorageInfo storage;
		uint64_t size;
		Collector collector;
		switch (operation) {
			case kPtpOpGetStorageIDs:
				session.GetStorageIDs(list);
				break;
			case kPtpOpGetObjectHandles:
				session.GetObjectHandles(0x00010001, 0, 0, list);
				break;
			case kPtpOpGetObjectInfo:
				session.GetObjectInfo(3, info);
				break;
			case kPtpOpGetObject:
				session.GetObject(4, collector.Sink(),
					random() % 2 == 0 ? kPtpSizeUnknown : 70001);
				break;
			case kPtpOpGetThumb:
				session.GetThumb(3, data);
				break;
			case kMtpOpGetObjectPropList:
				session.ListObjects(objects);
				break;
			case kPtpOpGetStorageInfo:
				session.GetStorageInfo(0x00010001, storage);
				break;
			case kMtpOpGetObjectPropValue:
				session.GetObjectSize(4, size);
				break;
		}
		// The device is in step again afterwards, or the session knows that
		// it is closed. (More noise may still be on its way when the call
		// returns: the next call meets it, fails and cleans up.)
		if (session.IsOpen() && session.GetStorageIDs(list) != kPtpOk
			&& session.IsOpen()) {
			PtpStatus status = session.GetStorageIDs(list);
			if (status != kPtpOk) {
				std::cerr << "round " << round << ": "
					<< PtpStatusText(status) << " " << session.LastError()
					<< "\n";
			}
			CHECK_EQ(status, kPtpOk);
		}
	}
}


// A property list whose counts and types lie.
TEST(PtpPropListDamage)
{
	FakeDevice device;
	device.mtp = true;
	device.propList = true;
	PtpSession session(device);
	CHECK_EQ(session.Open(), kPtpOk);
	std::mt19937 random(99);
	for (int round = 0; round < 300; round++) {
		std::vector<uint8_t> payload;
		W32(payload, random() % 3 == 0 ? (uint32_t)random() : random() % 40);
		size_t size = random() % 400;
		for (size_t i = 0; i < size; i++) {
			payload.push_back(random() % 4 == 0 ? 0xFF
				: random() % 4 == 0 ? 0 : (uint8_t)random());
		}
		// The storage list is asked first (transaction n), the property
		// list is n + 1: answer both.
		std::vector<uint8_t> bytes;
		W32(bytes, (uint32_t)(12 + payload.size()));
		W16(bytes, 2);
		W16(bytes, kMtpOpGetObjectPropList);
		W32(bytes, 0);
		bytes.insert(bytes.end(), payload.begin(), payload.end());
		std::vector<PtpObjectInfo> objects;
		// GetStorageIDs is answered by the device itself; then the noise
		// stands in for the property list's data.
		std::vector<uint32_t> ids;
		CHECK_EQ(session.GetStorageIDs(ids), kPtpOk);
		device.Push(bytes);
		session.ListObjects(objects);
		for (const PtpObjectInfo& info : objects)
			CHECK(!info.filename.empty());
	}
}
