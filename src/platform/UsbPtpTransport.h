// The PTP transport on the USB Kit: the bulk pipes of a still image (or MTP)
// interface, with timeouts.
#pragma once

#include "ptp/Ptp.h"

#include <OS.h>

#include <string>

class BUSBDevice;
class BUSBEndpoint;
class BUSBInterface;

namespace photos {

// Where a USB device speaks PTP or MTP.
struct UsbPtpLocation {
	uint32	configuration = 0;	// index for BUSBDevice::ConfigurationAt()
	uint32	interface = 0;		// index for BUSBConfiguration::InterfaceAt()
	bool	active = false;		// in the configuration that is set
	bool	mtp = false;		// found as MTP (a phone) rather than by class
};

// Looks for the still image interface (class 6, subclass 1, protocol 1) or
// an MTP interface on a vendor-specific class: one named "MTP", or, with
// askDevice, one Microsoft's OS descriptors call MTP (a question to the
// device, so only asked of interfaces that have the three pipes of MTP).
bool FindUsbPtpInterface(const BUSBDevice& device, UsbPtpLocation* location,
	bool askDevice = true);


class UsbPtpTransport : public PtpTransport {
public:
								UsbPtpTransport();
	virtual						~UsbPtpTransport();

	// path: the device under /dev/bus/usb.
			status_t			Open(const char* path);
			void				Close();
			bool				IsOpen() const { return fDevice != NULL; }
			const BUSBDevice*	Device() const { return fDevice; }
			bool				IsMtpInterface() const { return fLocation.mtp; }
	// The device was unplugged (or stopped answering for good).
			bool				IsGone();

	virtual	PtpStatus			BulkOut(const uint8_t* data, size_t length,
									int timeoutMs);
	virtual	PtpStatus			BulkIn(uint8_t* data, size_t capacity,
									size_t* received, int timeoutMs);
	virtual	size_t				MaxPacketIn() const { return fMaxPacketIn; }
	virtual	size_t				MaxPacketOut() const { return fMaxPacketOut; }
	virtual	PtpStatus			CancelRequest(uint32_t transactionId);
	virtual	PtpStatus			DeviceStatus();
	virtual	PtpStatus			ResetDevice();
	virtual	PtpStatus			ClearStalls();

private:
	struct Worker;

	static	int32				_WorkerEntry(void* data);
			status_t			_StartWorker();
			void				_StopWorker(bool kill);
			PtpStatus			_Run(int timeoutMs, ssize_t* result);
			PtpStatus			_Bulk(const BUSBEndpoint* endpoint,
									const uint8_t* out, uint8_t* in,
									size_t length, size_t* done,
									int timeoutMs);
			PtpStatus			_Control(uint8 requestType, uint8 request,
									uint16 value, uint16 index,
									const uint8* out, uint8* in,
									uint16 length, ssize_t* done);
			void				_AfterFailure();

			BUSBDevice*			fDevice;
			std::string			fPath;
			int					fProbe;
			bool				fGone;
			UsbPtpLocation		fLocation;
			const BUSBEndpoint*	fBulkIn;
			const BUSBEndpoint*	fBulkOut;
			uint16				fInterfaceNumber;
			size_t				fMaxPacketIn;
			size_t				fMaxPacketOut;
			Worker*				fWorker;
};

}	// namespace photos
