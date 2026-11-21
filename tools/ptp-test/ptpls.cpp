// ptpls: the camera layer from a shell.
//
//   ptpls                      the cameras, phones and cards that are there
//   ptpls --watch              arrivals and removals, until interrupted
//   ptpls info CAMERA          what the device says about itself (PTP only)
//   ptpls list CAMERA          its pictures and videos
//   ptpls thumbs CAMERA        the size of every thumbnail
//   ptpls thumb CAMERA ITEM FILE
//   ptpls get CAMERA ITEM|all DIRECTORY
//   ptpls delete CAMERA ITEM
//   ptpls cancel CAMERA ITEM BYTES   gives a download up after BYTES, then
//                              lists again (the device must be usable)
//
// CAMERA is a number from the list or a key; ITEM an item's id.
#include "platform/CameraSource.h"
#include "platform/UsbPtpTransport.h"
#include "ptp/Ptp.h"

#include <Application.h>
#include <USBKit.h>

#include <sys/stat.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace photos;


class WatchApp : public BApplication {
public:
	WatchApp()
		:
		BApplication("application/x-vnd.airos-Photos-ptpls")
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kMsgCameraArrived
			|| message->what == kMsgCameraRemoved) {
			printf("%s %s \"%s\" %s%s\n",
				message->what == kMsgCameraArrived ? "arrived" : "removed",
				message->GetString("key", "?"),
				message->GetString("name", "?"),
				message->GetInt32("kind", 0) == kCameraPtp ? "ptp" : "volume",
				message->GetBool("initial", false) ? " (initial)" : "");
			fflush(stdout);
			return;
		}
		BApplication::MessageReceived(message);
	}
};


static std::string
TimeText(int64_t seconds)
{
	if (seconds <= 0)
		return "-";
	time_t when = (time_t)seconds;
	struct tm fields;
	localtime_r(&when, &fields);
	char text[32];
	strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &fields);
	return text;
}


static CameraSource*
OpenCamera(CameraWatcher& watcher, const char* which)
{
	std::vector<CameraInfo> cameras = watcher.Cameras();
	std::string key = which;
	char* end;
	long index = strtol(which, &end, 10);
	if (*end == '\0' && index >= 0 && (size_t)index < cameras.size())
		key = cameras[index].key;
	CameraSource* source = watcher.CreateSource(key);
	if (source == NULL) {
		fprintf(stderr, "ptpls: no camera \"%s\"\n", which);
		return NULL;
	}
	std::string error;
	status_t status = source->Open(&error);
	if (status != B_OK) {
		fprintf(stderr, "ptpls: %s: %s (%s)\n", source->Info().name.c_str(),
			error.c_str(), strerror(status));
		delete source;
		return NULL;
	}
	return source;
}


static int
List(CameraSource* source, std::vector<CameraItem>& items, bool print)
{
	bigtime_t start = system_time();
	status_t status = source->ListItems(items, NULL, [](int32 found) {
		fprintf(stderr, "\r%d...", (int)found);
	});
	fprintf(stderr, "\r");
	if (status != B_OK) {
		fprintf(stderr, "ptpls: list: %s (%s)\n", source->LastError().c_str(),
			strerror(status));
		return 1;
	}
	if (print) {
		for (const CameraItem& item : items) {
			printf("%s\t%10lld\t%s\t%s%s\t%s/%s\n", item.id.c_str(),
				(long long)item.size, TimeText(item.taken).c_str(),
				item.isVideo ? "video" : item.isRaw ? "raw" : "picture", "",
				item.folder.c_str(), item.name.c_str());
		}
		fprintf(stderr, "%d items in %.2f s\n", (int)items.size(),
			(system_time() - start) / 1e6);
	}
	return 0;
}


static const CameraItem*
FindItem(const std::vector<CameraItem>& items, const char* id)
{
	for (const CameraItem& item : items) {
		if (item.id == id || item.name == id)
			return &item;
	}
	fprintf(stderr, "ptpls: no item \"%s\"\n", id);
	return NULL;
}


static int
Info(const char* which, CameraWatcher& watcher)
{
	// The device's own words, below the camera layer.
	std::vector<CameraInfo> cameras = watcher.Cameras();
	long index = strtol(which, NULL, 10);
	if (index < 0 || (size_t)index >= cameras.size()
		|| cameras[index].kind != kCameraPtp) {
		fprintf(stderr, "ptpls: info wants the number of a PTP camera\n");
		return 1;
	}
	// The key does not say where the device is: look for it again.
	for (int bus = 0; bus < 16; bus++) {
		for (int address = 0; address < 128; address++) {
			char path[64];
			snprintf(path, sizeof(path), "/dev/bus/usb/%d/%d", bus, address);
			struct stat st;
			if (stat(path, &st) != 0)
				continue;
			UsbPtpTransport transport;
			if (transport.Open(path) != B_OK)
				continue;
			const BUSBDevice* usb = transport.Device();
			printf("%s: %04x:%04x \"%s\" \"%s\" serial \"%s\", %s interface, "
				"packets %d in / %d out\n", path, usb->VendorID(),
				usb->ProductID(), usb->ManufacturerString(),
				usb->ProductString(), usb->SerialNumberString(),
				transport.IsMtpInterface() ? "MTP" : "still image",
				(int)transport.MaxPacketIn(), (int)transport.MaxPacketOut());
			PtpSession session(transport);
			PtpStatus status = session.Open();
			if (status != kPtpOk) {
				printf("  open: %s (%s)\n", PtpStatusText(status),
					session.LastError().c_str());
				continue;
			}
			const PtpDeviceInfo& info = session.DeviceInfo();
			printf("  manufacturer \"%s\" model \"%s\" version \"%s\" serial "
				"\"%s\"\n  standard %d, extension %u \"%s\", %s\n  operations:",
				info.manufacturer.c_str(), info.model.c_str(),
				info.deviceVersion.c_str(), info.serialNumber.c_str(),
				info.standardVersion, (unsigned)info.vendorExtensionId,
				info.vendorExtensionDescription.c_str(),
				info.IsMtp() ? "MTP" : "PTP");
			for (uint16_t operation : info.operations)
				printf(" %04x", operation);
			printf("\n");
			std::vector<uint32_t> storages;
			status = session.GetStorageIDs(storages);
			printf("  storages: %s", status == kPtpOk ? ""
				: PtpStatusText(status));
			for (uint32_t id : storages) {
				PtpStorageInfo storage;
				session.GetStorageInfo(id, storage);
				printf(" %08x \"%s\" \"%s\" %lld of %lld bytes free%s",
					(unsigned)id, storage.description.c_str(),
					storage.volumeLabel.c_str(), (long long)storage.freeSpace,
					(long long)storage.maxCapacity,
					storage.accessCapability != 0 ? ", read-only" : "");
			}
			printf("\n");
			session.Close();
		}
	}
	return 0;
}


int
main(int argc, char** argv)
{
	WatchApp app;
	bool watch = argc > 1 && strcmp(argv[1], "--watch") == 0;
	CameraWatcher watcher(watch ? BMessenger(&app) : BMessenger());
	status_t status = watcher.Start();
	if (status != B_OK) {
		fprintf(stderr, "ptpls: %s\n", strerror(status));
		return 1;
	}
	if (watch) {
		app.Run();
		watcher.Stop();
		return 0;
	}

	if (argc < 2) {
		std::vector<CameraInfo> cameras = watcher.Cameras();
		for (size_t i = 0; i < cameras.size(); i++) {
			printf("%d\t%s\t%s\t%s\n", (int)i,
				cameras[i].kind == kCameraPtp ? "ptp" : "volume",
				cameras[i].key.c_str(), cameras[i].name.c_str());
		}
		if (cameras.empty())
			fprintf(stderr, "no cameras\n");
		return 0;
	}

	const char* command = argv[1];
	if (argc < 3) {
		fprintf(stderr, "usage: ptpls [--watch | info|list|thumbs CAMERA | "
			"thumb CAMERA ITEM FILE | get CAMERA ITEM|all DIRECTORY | "
			"delete CAMERA ITEM | cancel CAMERA ITEM BYTES]\n");
		return 1;
	}
	if (strcmp(command, "info") == 0)
		return Info(argv[2], watcher);

	std::unique_ptr<CameraSource> source(OpenCamera(watcher, argv[2]));
	if (!source)
		return 1;
	fprintf(stderr, "%s (%s)\n", source->Info().name.c_str(),
		source->Info().key.c_str());
	std::vector<CameraItem> items;

	if (strcmp(command, "list") == 0)
		return List(source.get(), items, true);

	if (List(source.get(), items, false) != 0)
		return 1;

	if (strcmp(command, "thumbs") == 0) {
		int with = 0;
		bigtime_t start = system_time();
		for (const CameraItem& item : items) {
			std::vector<uint8_t> jpeg;
			status = source->GetThumbnail(item, jpeg);
			if (status == B_OK) {
				with++;
				printf("%s\t%d bytes\t%s\n", item.id.c_str(),
					(int)jpeg.size(), item.name.c_str());
			} else {
				printf("%s\t%s\t%s\n", item.id.c_str(), strerror(status),
					item.name.c_str());
			}
		}
		fprintf(stderr, "%d of %d with a thumbnail, %.2f s\n", with,
			(int)items.size(), (system_time() - start) / 1e6);
		return 0;
	}

	if (strcmp(command, "thumb") == 0 && argc >= 5) {
		const CameraItem* item = FindItem(items, argv[3]);
		if (item == NULL)
			return 1;
		std::vector<uint8_t> jpeg;
		status = source->GetThumbnail(*item, jpeg);
		if (status != B_OK) {
			fprintf(stderr, "ptpls: thumb: %s\n", strerror(status));
			return 1;
		}
		FILE* file = fopen(argv[4], "wb");
		if (file == NULL
			|| fwrite(jpeg.data(), 1, jpeg.size(), file) != jpeg.size()) {
			fprintf(stderr, "ptpls: %s: cannot write\n", argv[4]);
			return 1;
		}
		fclose(file);
		return 0;
	}

	if (strcmp(command, "get") == 0 && argc >= 5) {
		bool all = strcmp(argv[3], "all") == 0;
		mkdir(argv[4], 0755);
		int failed = 0;
		int64 bytes = 0;
		bigtime_t start = system_time();
		for (const CameraItem& item : items) {
			if (!all && item.id != argv[3] && item.name != argv[3])
				continue;
			std::string path = std::string(argv[4]) + "/" + item.name;
			status = source->Download(item, path.c_str(),
				[](int64 done, int64 total) { return true; });
			if (status != B_OK) {
				failed++;
				fprintf(stderr, "ptpls: %s: %s (%s)\n", item.name.c_str(),
					source->LastError().c_str(), strerror(status));
				continue;
			}
			bytes += item.size;
			printf("%s\n", path.c_str());
		}
		double seconds = (system_time() - start) / 1e6;
		fprintf(stderr, "%lld bytes in %.2f s (%.1f MiB/s), %d failed\n",
			(long long)bytes, seconds,
			seconds > 0 ? bytes / seconds / 1048576 : 0, failed);
		return failed == 0 ? 0 : 1;
	}

	if (strcmp(command, "delete") == 0 && argc >= 4) {
		const CameraItem* item = FindItem(items, argv[3]);
		if (item == NULL)
			return 1;
		status = source->DeleteItem(*item);
		if (status != B_OK) {
			fprintf(stderr, "ptpls: delete: %s (%s)\n",
				source->LastError().c_str(), strerror(status));
			return 1;
		}
		return 0;
	}

	if (strcmp(command, "cancel") == 0 && argc >= 5) {
		const CameraItem* item = FindItem(items, argv[3]);
		if (item == NULL)
			return 1;
		int64 limit = atoll(argv[4]);
		const char* path = "/tmp/ptpls-cancel.tmp";
		bigtime_t start = system_time();
		status = source->Download(*item, path,
			[limit](int64 done, int64 total) { return done < limit; });
		struct stat st;
		printf("download: %s after %.2f s, file %s\n", strerror(status),
			(system_time() - start) / 1e6,
			stat(path, &st) == 0 ? "left behind" : "removed");
		std::vector<CameraItem> again;
		start = system_time();
		status = source->ListItems(again, NULL, nullptr);
		printf("list afterwards: %s, %d items, %.2f s\n", strerror(status),
			(int)again.size(), (system_time() - start) / 1e6);
		return status == B_OK && again.size() == items.size() ? 0 : 1;
	}

	fprintf(stderr, "ptpls: unknown command \"%s\"\n", command);
	return 1;
}
