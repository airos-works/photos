#include "platform/UsbPtpTransport.h"

#include <USBKit.h>

#include <fcntl.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace photos {

namespace {

const uint8 kClassStillImage = 6;
const uint8 kClassVendor = 0xFF;

// The class requests of the still image capture device class.
const uint8 kRequestCancel = 0x64;
const uint8 kRequestDeviceReset = 0x66;
const uint8 kRequestGetDeviceStatus = 0x67;
const uint8 kClassInterfaceOut = USB_REQTYPE_CLASS | USB_REQTYPE_INTERFACE_OUT;
const uint8 kClassInterfaceIn = USB_REQTYPE_CLASS | USB_REQTYPE_INTERFACE_IN;


// PHOTOS_PTP_TRACE=1 in the environment: every transfer on stderr.
void
Trace(const char* format, ...)
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = getenv("PHOTOS_PTP_TRACE") != NULL ? 1 : 0;
	if (enabled == 0)
		return;
	va_list args;
	va_start(args, format);
	fputs("ptp: ", stderr);
	vfprintf(stderr, format, args);
	va_end(args);
}


bool
HasMtpPipes(const BUSBInterface* interface)
{
	int in = 0, out = 0, interrupt = 0;
	for (uint32 i = 0; i < interface->CountEndpoints(); i++) {
		const BUSBEndpoint* endpoint = interface->EndpointAt(i);
		if (endpoint == NULL)
			continue;
		if (endpoint->IsBulk() && endpoint->IsInput())
			in++;
		else if (endpoint->IsBulk() && endpoint->IsOutput())
			out++;
		else if (endpoint->IsInterrupt() && endpoint->IsInput())
			interrupt++;
	}
	return in == 1 && out == 1 && interrupt <= 1
		&& interface->CountEndpoints() <= 3;
}


// Microsoft's OS descriptors: string 0xEE says "MSFT100" and names a vendor
// request, whose answer lists a "compatible ID" per interface. MTP devices
// say "MTP" there.
bool
OsDescriptorSaysMtp(const BUSBDevice& device, uint8 interfaceNumber)
{
	uint8 text[18];
	memset(text, 0, sizeof(text));
	size_t length = device.GetDescriptor(USB_DESCRIPTOR_STRING, 0xEE, 0, text,
		sizeof(text));
	static const uint8 kSignature[14] = {'M', 0, 'S', 0, 'F', 0, 'T', 0,
		'1', 0, '0', 0, '0', 0};
	if (length < sizeof(text) || length > 4096
		|| memcmp(text + 2, kSignature, sizeof(kSignature)) != 0) {
		return false;
	}
	uint8 vendorRequest = text[16];

	uint8 ids[16 + 24 * 8];
	memset(ids, 0, sizeof(ids));
	ssize_t got = device.ControlTransfer(
		USB_REQTYPE_VENDOR | USB_REQTYPE_DEVICE_IN, vendorRequest, 0, 4,
		sizeof(ids), ids);
	if (got < 16 + 24)
		return false;
	uint32 count = ids[8];
	for (uint32 i = 0; i < count && 16 + (i + 1) * 24 <= (uint32)got; i++) {
		const uint8* function = ids + 16 + i * 24;
		if (memcmp(function + 2, "MTP", 4) == 0
			&& (function[0] == interfaceNumber || count == 1)) {
			return true;
		}
	}
	return false;
}


bool
FindInConfiguration(const BUSBDevice& device,
	const BUSBConfiguration* configuration, UsbPtpLocation* location,
	bool askDevice)
{
	if (configuration == NULL)
		return false;
	// The class says it outright.
	for (uint32 i = 0; i < configuration->CountInterfaces(); i++) {
		const BUSBInterface* interface = configuration->InterfaceAt(i);
		if (interface == NULL)
			continue;
		if (interface->Class() == kClassStillImage
			&& interface->Subclass() == 1 && interface->Protocol() == 1
			&& HasMtpPipes(interface)) {
			location->interface = i;
			location->mtp = false;
			return true;
		}
	}
	// A phone in "file transfer" mode: vendor-specific, named.
	for (uint32 i = 0; i < configuration->CountInterfaces(); i++) {
		const BUSBInterface* interface = configuration->InterfaceAt(i);
		if (interface == NULL || !HasMtpPipes(interface))
			continue;
		if (interface->Class() != kClassVendor
			&& interface->Class() != kClassStillImage) {
			continue;
		}
		const char* name = interface->InterfaceString();
		if (name != NULL && strcasestr(name, "MTP") != NULL) {
			location->interface = i;
			location->mtp = true;
			return true;
		}
	}
	if (!askDevice)
		return false;
	for (uint32 i = 0; i < configuration->CountInterfaces(); i++) {
		const BUSBInterface* interface = configuration->InterfaceAt(i);
		if (interface == NULL || interface->Class() != kClassVendor
			|| interface->Subclass() != 0xFF || !HasMtpPipes(interface)
			|| interface->CountEndpoints() != 3) {
			continue;
		}
		const usb_interface_descriptor* descriptor = interface->Descriptor();
		if (descriptor != NULL
			&& OsDescriptorSaysMtp(device, descriptor->interface_number)) {
			location->interface = i;
			location->mtp = true;
			return true;
		}
	}
	return false;
}

}	// namespace


bool
FindUsbPtpInterface(const BUSBDevice& device, UsbPtpLocation* location,
	bool askDevice)
{
	if (device.IsHub())
		return false;
	UsbPtpLocation found;
	const BUSBConfiguration* active = device.ActiveConfiguration();
	if (active != NULL
		&& FindInConfiguration(device, active, &found, askDevice)) {
		found.configuration = active->Index();
		found.active = true;
		if (location != NULL)
			*location = found;
		return true;
	}
	// A device with several configurations may keep its pictures in another
	// one than the one that is set.
	for (uint32 i = 0; i < device.CountConfigurations(); i++) {
		const BUSBConfiguration* configuration = device.ConfigurationAt(i);
		if (configuration == NULL || configuration == active)
			continue;
		if (FindInConfiguration(device, configuration, &found, false)) {
			found.configuration = i;
			found.active = false;
			if (location != NULL)
				*location = found;
			return true;
		}
	}
	return false;
}

namespace {

// One transfer carries at most this much (the session asks for 256 KiB).
const size_t kIoBufferSize = 256 * 1024;
const int kControlTimeoutMs = 5000;
// usb_raw's "which protocol version" ioctl (B_USB_RAW_COMMAND_GET_VERSION,
// what BUSBDevice::SetTo() asks first): answered by the driver itself.
const uint32 kUsbRawGetVersion = 0x1000;

}	// namespace


// What the transfer thread works with. On the heap and apart from the
// transport: a thread that is stuck in the kernel for good is left behind
// with it (see _StopWorker()).
struct UsbPtpTransport::Worker {
	thread_id			thread = -1;
	sem_id				request = -1;
	sem_id				done = -1;
	bool				quit = false;

	// the request
	BUSBDevice*			device = NULL;
	const BUSBEndpoint*	endpoint = NULL;	// a bulk transfer, or a control
	uint8				requestType = 0;	// request on the default pipe
	uint8				requestCode = 0;
	uint16				value = 0;
	uint16				index = 0;
	size_t				length = 0;
	ssize_t				result = 0;
	int32				finished = 0;
	uint8				buffer[kIoBufferSize];

	void Run()
	{
		while (true) {
			status_t status;
			do {
				status = acquire_sem(request);
			} while (status == B_INTERRUPTED);
			if (status != B_OK || quit)
				return;
			if (endpoint != NULL)
				result = endpoint->BulkTransfer(buffer, length);
			else {
				result = device->ControlTransfer(requestType, requestCode,
					value, index, (uint16)length, buffer);
			}
			atomic_set(&finished, 1);
			release_sem(done);
		}
	}
};


UsbPtpTransport::UsbPtpTransport()
	:
	fDevice(NULL),
	fProbe(-1),
	fGone(false),
	fBulkIn(NULL),
	fBulkOut(NULL),
	fInterfaceNumber(0),
	fMaxPacketIn(512),
	fMaxPacketOut(512),
	fWorker(NULL)
{
}


UsbPtpTransport::~UsbPtpTransport()
{
	Close();
}


status_t
UsbPtpTransport::Open(const char* path)
{
	Close();
	BUSBDevice* device = new(std::nothrow) BUSBDevice(path);
	if (device == NULL)
		return B_NO_MEMORY;
	if (device->InitCheck() != B_OK) {
		delete device;
		return B_DEV_NOT_READY;
	}
	UsbPtpLocation location;
	if (!FindUsbPtpInterface(*device, &location)) {
		delete device;
		return B_DEVICE_NOT_FOUND;
	}
	const BUSBConfiguration* configuration
		= device->ConfigurationAt(location.configuration);
	if (configuration == NULL) {
		delete device;
		return B_DEVICE_NOT_FOUND;
	}
	if (!location.active) {
		status_t status = device->SetConfiguration(configuration);
		if (status != B_OK) {
			delete device;
			return status;
		}
	}
	const BUSBInterface* interface
		= configuration->InterfaceAt(location.interface);
	if (interface == NULL) {
		delete device;
		return B_DEVICE_NOT_FOUND;
	}

	fBulkIn = fBulkOut = NULL;
	for (uint32 i = 0; i < interface->CountEndpoints(); i++) {
		const BUSBEndpoint* endpoint = interface->EndpointAt(i);
		if (endpoint == NULL)
			continue;
		if (endpoint->IsBulk() && endpoint->IsInput())
			fBulkIn = endpoint;
		else if (endpoint->IsBulk() && endpoint->IsOutput())
			fBulkOut = endpoint;
	}
	if (fBulkIn == NULL || fBulkOut == NULL) {
		delete device;
		return B_DEVICE_NOT_FOUND;
	}

	fDevice = device;
	fPath = path;
	fGone = false;
	// A second way to the same device, to ask whether it is still there
	// without sending it anything.
	fProbe = open(path, O_RDWR | O_CLOEXEC);
	fLocation = location;
	const usb_interface_descriptor* descriptor = interface->Descriptor();
	fInterfaceNumber = descriptor != NULL ? descriptor->interface_number
		: location.interface;
	fMaxPacketIn = fBulkIn->MaxPacketSize();
	fMaxPacketOut = fBulkOut->MaxPacketSize();
	if (fMaxPacketIn == 0)
		fMaxPacketIn = 512;
	if (fMaxPacketOut == 0)
		fMaxPacketOut = 512;

	// Pipes another program left halted.
	ClearStalls();
	return B_OK;
}


void
UsbPtpTransport::Close()
{
	_StopWorker(false);
	// A thread that never came back still uses the device object: then
	// _StopWorker() has taken it away.
	delete fDevice;
	fDevice = NULL;
	fBulkIn = fBulkOut = NULL;
	if (fProbe >= 0)
		close(fProbe);
	fProbe = -1;
}


bool
UsbPtpTransport::IsGone()
{
	if (fGone || fDevice == NULL)
		return true;
	// usb_raw answers this by itself, and not at all once the device has
	// been removed.
	status_t version = 0;
	if (fProbe < 0
		|| ioctl(fProbe, kUsbRawGetVersion, &version, sizeof(version)) != 0) {
		Trace("the device is gone\n");
		fGone = true;
	}
	return fGone;
}


// After a transfer failed: time for the system to notice that the device
// was unplugged, if that is why. (A request sent to a device while usb_raw
// is taking it away can stay queued for ever, with the thread that waits
// for it; a moment later usb_raw refuses requests by itself.)
void
UsbPtpTransport::_AfterFailure()
{
	snooze(150000);
	IsGone();
}


int32
UsbPtpTransport::_WorkerEntry(void* data)
{
	static_cast<Worker*>(data)->Run();
	return 0;
}


status_t
UsbPtpTransport::_StartWorker()
{
	if (fWorker != NULL)
		return B_OK;
	Worker* worker = new(std::nothrow) Worker;
	if (worker == NULL)
		return B_NO_MEMORY;
	worker->request = create_sem(0, "ptp request");
	worker->done = create_sem(0, "ptp done");
	worker->thread = spawn_thread(_WorkerEntry, "ptp transfer",
		B_NORMAL_PRIORITY, worker);
	if (worker->request < 0 || worker->done < 0 || worker->thread < 0) {
		delete_sem(worker->request);
		delete_sem(worker->done);
		if (worker->thread >= 0)
			kill_thread(worker->thread);
		delete worker;
		return B_NO_MORE_THREADS;
	}
	resume_thread(worker->thread);
	fWorker = worker;
	return B_OK;
}


void
UsbPtpTransport::_StopWorker(bool kill)
{
	Worker* worker = fWorker;
	if (worker == NULL)
		return;
	fWorker = NULL;
	if (kill) {
		// In the kernel the thread waits for its transfer "until killed":
		// that cancels the transfer.
		kill_thread(worker->thread);
	} else {
		worker->quit = true;
		release_sem(worker->request);
	}
	status_t result;
	status_t status;
	do {
		status = wait_for_thread_etc(worker->thread, B_RELATIVE_TIMEOUT,
			3000000, &result);
	} while (status == B_INTERRUPTED);
	if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) {
		// It does not come back: the kernel keeps it. It goes on using the
		// device object and its buffer, so they stay; nothing more can be
		// done with this device.
		Trace("the transfer thread is stuck\n");
		fGone = true;
		fDevice = NULL;
		fBulkIn = fBulkOut = NULL;
		return;
	}
	delete_sem(worker->request);
	delete_sem(worker->done);
	delete worker;
}


// Hands the request that is set up in fWorker to the thread and waits for
// it.
PtpStatus
UsbPtpTransport::_Run(int timeoutMs, ssize_t* result)
{
	Worker* worker = fWorker;
	worker->result = B_ERROR;
	atomic_set(&worker->finished, 0);
	release_sem(worker->request);

	status_t status;
	bigtime_t end = system_time()
		+ (bigtime_t)(timeoutMs > 0 ? timeoutMs : 1) * 1000;
	do {
		status = acquire_sem_etc(worker->done, 1, B_ABSOLUTE_TIMEOUT, end);
	} while (status == B_INTERRUPTED);
	if (status != B_OK) {
		// What a transfer that came back at this very moment brought is
		// lost with the thread: the session treats a timeout as the end of
		// the transaction anyway.
		_StopWorker(true);
		if (!fGone)
			IsGone();
		return fGone ? kPtpNoDevice : kPtpTimeout;
	}
	*result = worker->result;
	return kPtpOk;
}


PtpStatus
UsbPtpTransport::_Bulk(const BUSBEndpoint* endpoint, const uint8_t* out,
	uint8_t* in, size_t length, size_t* done, int timeoutMs)
{
	*done = 0;
	if (fGone || fDevice == NULL || endpoint == NULL)
		return kPtpNoDevice;
	if (length > kIoBufferSize)
		length = kIoBufferSize;
	if (_StartWorker() != B_OK)
		return kPtpIoError;

	fWorker->endpoint = endpoint;
	fWorker->length = length;
	if (out != NULL)
		memcpy(fWorker->buffer, out, length);

	bigtime_t start = system_time();
	ssize_t result = B_ERROR;
	PtpStatus status = _Run(timeoutMs, &result);
	const char* direction = in != NULL ? "in" : "out";
	if (status != kPtpOk) {
		Trace("bulk %s %zu: %s after %d ms\n", direction, length,
			status == kPtpTimeout ? "timeout" : "gone", timeoutMs);
		return status;
	}
	if (result < 0 || (size_t)result > length) {
		Trace("bulk %s %zu: failed\n", direction, length);
		_AfterFailure();
		return fGone ? kPtpNoDevice : kPtpIoError;
	}
	if (in != NULL && fWorker != NULL)
		memcpy(in, fWorker->buffer, (size_t)result);
	*done = (size_t)result;
	Trace("bulk %s %zu: %zu, %lld us\n", direction, length, *done,
		(long long)(system_time() - start));
	return kPtpOk;
}


// A request on the default pipe. kPtpNotSupported: the device refused it.
PtpStatus
UsbPtpTransport::_Control(uint8 requestType, uint8 request, uint16 value,
	uint16 index, const uint8* out, uint8* in, uint16 length, ssize_t* done)
{
	*done = 0;
	if (fGone || fDevice == NULL)
		return kPtpNoDevice;
	if (_StartWorker() != B_OK)
		return kPtpIoError;

	fWorker->endpoint = NULL;
	fWorker->device = fDevice;
	fWorker->requestType = requestType;
	fWorker->requestCode = request;
	fWorker->value = value;
	fWorker->index = index;
	fWorker->length = length;
	memset(fWorker->buffer, 0, length);
	if (out != NULL)
		memcpy(fWorker->buffer, out, length);

	ssize_t result = B_ERROR;
	PtpStatus status = _Run(kControlTimeoutMs, &result);
	Trace("control %02x %02x value %04x index %04x length %d: %d%s\n",
		requestType, request, value, index, length, (int)result,
		status == kPtpTimeout ? " (timeout)" : "");
	if (status != kPtpOk)
		return status;
	if (result < 0 || result > length) {
		_AfterFailure();
		return fGone ? kPtpNoDevice : kPtpNotSupported;
	}
	if (in != NULL && fWorker != NULL)
		memcpy(in, fWorker->buffer, (size_t)result);
	*done = result;
	return kPtpOk;
}


PtpStatus
UsbPtpTransport::BulkOut(const uint8_t* data, size_t length, int timeoutMs)
{
	size_t sent = 0;
	while (sent < length) {
		size_t done = 0;
		size_t part = std::min(length - sent, kIoBufferSize);
		PtpStatus status = _Bulk(fBulkOut, data + sent, NULL, part, &done,
			timeoutMs);
		if (status != kPtpOk)
			return status;
		if (done != part)
			return kPtpIoError;
		sent += done;
	}
	if (length > 0 && length % fMaxPacketOut == 0) {
		// A transfer of whole packets ends with an empty one.
		size_t done = 0;
		uint8 nothing = 0;
		_Bulk(fBulkOut, &nothing, NULL, 0, &done, timeoutMs);
	}
	return kPtpOk;
}


PtpStatus
UsbPtpTransport::BulkIn(uint8_t* data, size_t capacity, size_t* received,
	int timeoutMs)
{
	return _Bulk(fBulkIn, NULL, data, capacity, received, timeoutMs);
}


PtpStatus
UsbPtpTransport::CancelRequest(uint32_t transactionId)
{
	// "Cancel transaction" and which one.
	uint8 data[6] = {0x01, 0x40, (uint8)transactionId,
		(uint8)(transactionId >> 8), (uint8)(transactionId >> 16),
		(uint8)(transactionId >> 24)};
	ssize_t done;
	return _Control(kClassInterfaceOut, kRequestCancel, 0, fInterfaceNumber,
		data, NULL, sizeof(data), &done);
}


PtpStatus
UsbPtpTransport::DeviceStatus()
{
	uint8 data[32];
	ssize_t result;
	PtpStatus status = _Control(kClassInterfaceIn, kRequestGetDeviceStatus, 0,
		fInterfaceNumber, NULL, data, sizeof(data), &result);
	if (status != kPtpOk)
		return status;
	// The answer says how long it is and carries a response code. A device
	// that does not know the request may "answer" with whatever was in its
	// buffer (QEMU's usb-mtp does): that is no status.
	if (result < 4)
		return kPtpNotSupported;
	uint16 size = data[0] | (data[1] << 8);
	uint16 code = data[2] | (data[3] << 8);
	if (size < 4 || size > result || (code & 0xF000) != 0x2000)
		return kPtpNotSupported;
	if (code == kPtpRespOk)
		return kPtpOk;
	if (code == kPtpRespDeviceBusy)
		return kPtpBusy;
	// "Transaction cancelled", or pipes that are halted: the caller clears
	// them and asks again.
	ClearStalls();
	return kPtpBusy;
}


PtpStatus
UsbPtpTransport::ResetDevice()
{
	ssize_t done;
	PtpStatus status = _Control(kClassInterfaceOut, kRequestDeviceReset, 0,
		fInterfaceNumber, NULL, NULL, 0, &done);
	return status == kPtpNotSupported ? kPtpIoError : status;
}


PtpStatus
UsbPtpTransport::ClearStalls()
{
	if (fGone || fDevice == NULL)
		return kPtpNoDevice;
	const BUSBEndpoint* endpoints[2] = {fBulkIn, fBulkOut};
	for (const BUSBEndpoint* endpoint : endpoints) {
		if (endpoint == NULL || endpoint->Descriptor() == NULL)
			continue;
		uint16 address = endpoint->Descriptor()->endpoint_address;
		// GET_STATUS of the pipe: bit 0 is "halted".
		uint8 halted[2] = {0, 0};
		ssize_t done;
		PtpStatus status = _Control(USB_REQTYPE_ENDPOINT_IN,
			USB_REQUEST_GET_STATUS, 0, address, NULL, halted, sizeof(halted),
			&done);
		if (status == kPtpNoDevice || status == kPtpTimeout)
			return status;
		if (status == kPtpOk && done >= 1 && (halted[0] & 1) != 0) {
			_Control(USB_REQTYPE_ENDPOINT_OUT, USB_REQUEST_CLEAR_FEATURE,
				USB_FEATURE_ENDPOINT_HALT, address, NULL, NULL, 0, &done);
		}
	}
	return fGone ? kPtpNoDevice : kPtpOk;
}

}	// namespace photos
