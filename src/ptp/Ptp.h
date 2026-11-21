// A PTP/MTP initiator: what a computer says to a camera or a phone to list
// and fetch its pictures (ISO 15740, USB still image capture device class,
// and the MTP extensions phones use). Portable: the bytes travel through a
// PtpTransport, which is USB in the application (platform/UsbPtpTransport)
// and a fake device in the tests.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace photos {

enum PtpStatus {
	kPtpOk = 0,
	kPtpIoError,			// the transport failed
	kPtpTimeout,			// the device did not answer in time
	kPtpNoDevice,			// the device is gone
	kPtpProtocol,			// the device sent something that is not PTP
	kPtpCancelled,			// the caller's sink or cancel flag stopped it
	kPtpAccessDenied,		// locked, or access to pictures not allowed yet
	kPtpBusy,				// the device says it is busy
	kPtpNotSupported,		// the device does not know the operation
	kPtpStoreUnavailable,	// no card, or the storage went away
	kPtpInvalidObject,		// no such object (any more)
	kPtpReadOnly,			// the object or the storage is write-protected
	kPtpNoSession,			// Open() was not called or failed
	kPtpFailed				// another response code: see LastResponse()
};

// Words for a status, for a person ("The device is locked...").
const char* PtpStatusText(PtpStatus status);
// The name of a response code ("Device_Busy"), or "0x2xxx".
std::string PtpResponseName(uint16_t code);


// The bulk pipes of a device. One transfer at a time; a session never calls
// from two threads at once.
class PtpTransport {
public:
	virtual						~PtpTransport() {}

	// Sends length bytes as one transfer.
	virtual	PtpStatus			BulkOut(const uint8_t* data, size_t length,
									int timeoutMs) = 0;
	// Receives one transfer: it ends when capacity bytes arrived or with a
	// packet shorter than MaxPacketIn(); *received may be 0 (a zero-length
	// packet). capacity is a multiple of MaxPacketIn() or the exact rest of
	// what the device has announced.
	virtual	PtpStatus			BulkIn(uint8_t* data, size_t capacity,
									size_t* received, int timeoutMs) = 0;
	virtual	size_t				MaxPacketIn() const = 0;
	virtual	size_t				MaxPacketOut() const = 0;

	// The class requests of the still image class; optional.
	// Cancel: the transaction that is in its data phase is given up.
	virtual	PtpStatus			CancelRequest(uint32_t transactionId)
									{ return kPtpNotSupported; }
	// Device status: kPtpOk when the device is ready for a command again,
	// kPtpBusy while it is not.
	virtual	PtpStatus			DeviceStatus() { return kPtpNotSupported; }
	// Device reset: the session is closed, the pipes are cleared.
	virtual	PtpStatus			ResetDevice() { return kPtpNotSupported; }
	// Clears halted pipes after a failed transfer.
	virtual	PtpStatus			ClearStalls() { return kPtpNotSupported; }
};


// Operation, response, format and property codes that callers meet.
enum {
	kPtpOpGetDeviceInfo = 0x1001,
	kPtpOpOpenSession = 0x1002,
	kPtpOpCloseSession = 0x1003,
	kPtpOpGetStorageIDs = 0x1004,
	kPtpOpGetStorageInfo = 0x1005,
	kPtpOpGetNumObjects = 0x1006,
	kPtpOpGetObjectHandles = 0x1007,
	kPtpOpGetObjectInfo = 0x1008,
	kPtpOpGetObject = 0x1009,
	kPtpOpGetThumb = 0x100A,
	kPtpOpDeleteObject = 0x100B,
	kPtpOpGetPartialObject = 0x101B,
	kMtpOpGetObjectPropsSupported = 0x9801,
	kMtpOpGetObjectPropValue = 0x9803,
	kMtpOpGetObjectPropList = 0x9805,

	kPtpRespOk = 0x2001,
	kPtpRespGeneralError = 0x2002,
	kPtpRespSessionNotOpen = 0x2003,
	kPtpRespInvalidTransactionId = 0x2004,
	kPtpRespOperationNotSupported = 0x2005,
	kPtpRespParameterNotSupported = 0x2006,
	kPtpRespIncompleteTransfer = 0x2007,
	kPtpRespInvalidStorageId = 0x2008,
	kPtpRespInvalidObjectHandle = 0x2009,
	kPtpRespStoreFull = 0x200C,
	kPtpRespObjectWriteProtected = 0x200D,
	kPtpRespStoreReadOnly = 0x200E,
	kPtpRespAccessDenied = 0x200F,
	kPtpRespNoThumbnailPresent = 0x2010,
	kPtpRespStoreNotAvailable = 0x2013,
	kPtpRespSpecByFormatUnsupported = 0x2014,
	kPtpRespDeviceBusy = 0x2019,
	kPtpRespInvalidParameter = 0x201D,
	kPtpRespSessionAlreadyOpen = 0x201E,
	kPtpRespTransactionCancelled = 0x201F,

	kPtpFormatUndefined = 0x3000,
	kPtpFormatAssociation = 0x3001,	// a folder
	kPtpFormatAvi = 0x300A,
	kPtpFormatMpeg = 0x300B,
	kPtpFormatAsf = 0x300C,
	kPtpFormatQuickTime = 0x300D,
	kPtpFormatJpeg = 0x3801,		// EXIF/JPEG
	kPtpFormatTiffEp = 0x3802,
	kPtpFormatBmp = 0x3804,
	kPtpFormatGif = 0x3807,
	kPtpFormatJfif = 0x3808,
	kPtpFormatPng = 0x380B,
	kPtpFormatTiff = 0x380D,
	kPtpFormatJp2 = 0x380F,
	kPtpFormatDng = 0x3811,
	kPtpFormatHeif = 0x3812,
	kMtpFormatWmv = 0xB981,
	kMtpFormatMp4 = 0xB982,
	kMtpFormatMp2 = 0xB983,
	kMtpFormat3gp = 0xB984,

	kMtpPropStorageId = 0xDC01,
	kMtpPropObjectFormat = 0xDC02,
	kMtpPropObjectSize = 0xDC04,
	kMtpPropObjectFileName = 0xDC07,
	kMtpPropDateCreated = 0xDC08,
	kMtpPropDateModified = 0xDC09,
	kMtpPropParentObject = 0xDC0B,

	kPtpAllStorages = 0xFFFFFFFF,
	kPtpRootParent = 0xFFFFFFFF
};


struct PtpDeviceInfo {
	uint16_t				standardVersion = 0;
	uint32_t				vendorExtensionId = 0;
	uint16_t				vendorExtensionVersion = 0;
	std::string				vendorExtensionDescription;
	uint16_t				functionalMode = 0;
	std::vector<uint16_t>	operations;
	std::vector<uint16_t>	events;
	std::vector<uint16_t>	deviceProperties;
	std::vector<uint16_t>	captureFormats;
	std::vector<uint16_t>	imageFormats;
	std::string				manufacturer;
	std::string				model;
	std::string				deviceVersion;
	std::string				serialNumber;

	bool					Supports(uint16_t operation) const;
	// MTP's own operations are there (a phone, a media player).
	bool					IsMtp() const;
};

struct PtpStorageInfo {
	uint16_t				storageType = 0;
	uint16_t				filesystemType = 0;
	uint16_t				accessCapability = 0;	// 0 read-write
	uint64_t				maxCapacity = 0;
	uint64_t				freeSpace = 0;
	uint32_t				freeObjects = 0;
	std::string				description;
	std::string				volumeLabel;
};

struct PtpObjectInfo {
	uint32_t				handle = 0;
	uint32_t				storageId = 0;
	uint16_t				format = 0;
	uint16_t				protection = 0;
	uint64_t				size = 0;				// bytes; kPtpSizeUnknown if not said
	uint16_t				thumbFormat = 0;
	uint32_t				thumbSize = 0;
	uint32_t				thumbWidth = 0;
	uint32_t				thumbHeight = 0;
	uint32_t				imageWidth = 0;
	uint32_t				imageHeight = 0;
	uint32_t				parent = 0;				// 0: the storage's root
	uint16_t				associationType = 0;
	std::string				filename;
	// Dates as PtpParseDate() gives them, 0 unknown. Cameras write local
	// times without a zone: those count seconds as if the wall-clock time
	// were UTC, and ...IsUtc is false.
	int64_t					captured = 0;
	int64_t					modified = 0;
	bool					capturedIsUtc = false;
	bool					modifiedIsUtc = false;

	bool					IsFolder() const
								{ return format == kPtpFormatAssociation; }
};

const uint64_t kPtpSizeUnknown = UINT64_MAX;

// Receives the data of an object in pieces, in order. total is the object's
// size or kPtpSizeUnknown. Returning false gives the transfer up.
typedef std::function<bool(const uint8_t* data, size_t length, uint64_t total)>
	PtpDataSink;


class PtpSession {
public:
								PtpSession(PtpTransport& transport);
								~PtpSession();

	// GetDeviceInfo and OpenSession. A session a crashed program left open
	// is taken over.
			PtpStatus			Open();
			PtpStatus			Close();
			bool				IsOpen() const { return fOpen; }
			const PtpDeviceInfo& DeviceInfo() const { return fDeviceInfo; }

			PtpStatus			GetStorageIDs(std::vector<uint32_t>& ids);
			PtpStatus			GetStorageInfo(uint32_t storageId,
									PtpStorageInfo& info);
	// format 0: every format; parent 0: everything on the storage,
	// kPtpRootParent: what is in its root.
			PtpStatus			GetObjectHandles(uint32_t storageId,
									uint16_t format, uint32_t parent,
									std::vector<uint32_t>& handles);
			PtpStatus			GetObjectInfo(uint32_t handle,
									PtpObjectInfo& info);
	// The whole object into the sink. sizeHint (from the listing) is used
	// when the device does not say how much is coming (objects of 4 GiB and
	// more).
			PtpStatus			GetObject(uint32_t handle,
									const PtpDataSink& sink,
									uint64_t sizeHint = kPtpSizeUnknown);
	// Up to maxBytes from offset (offset below 4 GiB).
			PtpStatus			GetPartialObject(uint32_t handle,
									uint32_t offset, uint32_t maxBytes,
									std::vector<uint8_t>& data);
			PtpStatus			GetThumb(uint32_t handle,
									std::vector<uint8_t>& data);
			PtpStatus			DeleteObject(uint32_t handle);
	// MTP: the size of an object of 4 GiB or more.
			PtpStatus			GetObjectSize(uint32_t handle, uint64_t& size);

	// Every object of the device (folders too: IsFolder()), by the fastest
	// way the device has: MTP's GetObjectPropList in one go, else handles
	// and one GetObjectInfo each. A locked phone that shows no storage
	// gives kPtpAccessDenied. progress is told how many are known so far.
			PtpStatus			ListObjects(std::vector<PtpObjectInfo>& objects,
									const std::atomic<bool>* cancel = nullptr,
									const std::function<void(int found)>& progress
										= nullptr);

	// The response code of the last operation, and its name with the
	// operation's, for logs: "GetObject: Invalid_ObjectHandle".
			uint16_t			LastResponse() const { return fLastResponse; }
			std::string			LastError() const;

	// How long a device may take to answer (default 20 s; listing a phone
	// full of pictures gets five times that).
			void				SetTimeout(int milliseconds)
									{ fTimeoutMs = milliseconds; }

private:
	struct Result;

			PtpStatus			_Transaction(uint16_t operation,
									const uint32_t* params, int paramCount,
									const PtpDataSink* sink, Result& result,
									uint64_t sizeHint = kPtpSizeUnknown,
									int timeoutFactor = 1);
			PtpStatus			_Simple(uint16_t operation,
									const uint32_t* params, int paramCount,
									std::vector<uint8_t>* data, Result& result,
									size_t dataLimit, int timeoutFactor = 1);
			PtpStatus			_ReadDataAndResponse(uint16_t operation,
									uint32_t transactionId,
									const PtpDataSink* sink, Result& result,
									uint64_t sizeHint, int timeoutMs);
			PtpStatus			_SkipData(uint32_t length, size_t received,
									size_t packet);
			PtpStatus			_ReadResponse(const uint8_t* first,
									size_t firstLength, uint32_t transactionId,
									Result& result, int timeoutMs);
			PtpStatus			_Recover(uint32_t transactionId,
									bool dataPending);
			PtpStatus			_Drain();
			PtpStatus			_StatusOf(uint16_t response) const;
			PtpStatus			_ListByPropList(
									std::vector<PtpObjectInfo>& objects,
									const std::atomic<bool>* cancel,
									const std::function<void(int)>& progress);
			PtpStatus			_ListByHandles(
									const std::vector<uint32_t>& storages,
									std::vector<PtpObjectInfo>& objects,
									const std::atomic<bool>* cancel,
									const std::function<void(int)>& progress);

			PtpTransport&		fTransport;
			PtpDeviceInfo		fDeviceInfo;
			bool				fOpen;
			uint32_t			fSessionId;
			uint32_t			fTransactionId;
			uint16_t			fLastResponse;
			uint16_t			fLastOperation;
			int					fTimeoutMs;
			std::vector<uint8_t> fBuffer;
};


// "20261121T143000", "20261121T143000.5", "...Z", "...+1100": seconds since
// the epoch when the text names its zone (*isUtc true); for a time without a
// zone, the seconds that wall-clock time would be in UTC (*isUtc false: the
// caller knows which zone the device was in, or assumes its own). 0 if the
// text is not a date.
int64_t PtpParseDate(const std::string& text, bool* isUtc = nullptr);

// The thumbnail a camera put into a JPEG's EXIF block (IFD1), found in the
// beginning of the file (64 KiB are enough). False if there is none.
bool ExifThumbnail(const uint8_t* data, size_t length,
	std::vector<uint8_t>& jpeg);

// What a file is, from its name: a picture, a camera RAW file, a video.
enum PtpMediaKind { kPtpMediaOther, kPtpMediaPicture, kPtpMediaRaw, kPtpMediaVideo };
PtpMediaKind PtpMediaKindOf(const std::string& filename, uint16_t format = 0);

}	// namespace photos
