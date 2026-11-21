# Cameras, phones and memory cards

Photos imports from two kinds of sources:

- **PTP/MTP devices** over USB: cameras in "PTP" or "PC connection" mode,
  iPhones and iPads (PTP), Android phones in "File transfer" (MTP) or "PTP"
  mode. Photos talks to them itself; nothing is mounted.
- **Volumes with a `DCIM` folder**: memory cards in a reader, cameras that
  show up as a disk.

`photos_agent` runs in the background from login and opens Photos when one
of them is connected.

## The pieces

| File | What |
| --- | --- |
| `src/ptp/Ptp.h`, `Ptp.cpp` | the PTP/MTP initiator (portable): `PtpSession` over a `PtpTransport` |
| `src/platform/UsbPtpTransport.*` | the transport on the USB Kit (`BUSBDevice`, usb_raw), and `FindUsbPtpInterface()` |
| `src/platform/CameraSource.*` | `CameraSource` (list, thumbnail, download, delete) for both kinds; `CameraWatcher` (arrivals and removals) |
| `src/agent/Agent.cpp`, `resources/photos_agent.launch` | the agent and its launch_daemon job |
| `tools/ptp-test/` | `ptpls` (the layer from a shell), fixtures and the VM tests |
| `tests/PtpTests.cpp` | host tests of the initiator against a made-up device |

### PtpSession

Operations: GetDeviceInfo, OpenSession (a session another program left open
is closed and reopened), CloseSession, GetStorageIDs, GetStorageInfo,
GetObjectHandles, GetObjectInfo, GetObject (into a sink, in 256 KiB pieces),
GetPartialObject, GetThumb, DeleteObject, and MTP's GetObjectPropList and
GetObjectPropValue (object size).

`ListObjects()` takes the fastest way a device has:

1. MTP `GetObjectPropList` for everything in one transfer (Android);
2. else folder by folder from the root, one `GetObjectInfo` per object,
   entering only the folders the caller's filter accepts;
3. else, for devices that cannot list a single folder, every handle of the
   storage and its `GetObjectInfo`.

Things devices do that it copes with (each has a host test): the data
header as a packet of its own; data that ends exactly on a packet boundary
with or without the zero-length packet the standard asks for; data
containers without a length (objects of 4 GiB and more: the size comes from
the MTP object property); `Device_Busy` (asked again up to five times);
responses of an earlier transaction that arrive late; a device that answers
"everything on the storage" with the root only (QEMU's usb-mtp); truncated
and nonsensical containers (no crash, no read outside a buffer: the tests
run under ASan and UBSan with random input).

After a transaction is given up (the sink said stop, a timeout, nonsense)
the session sends the class's Cancel request, throws away what still
arrives, asks the device's status, and if that does not get the device back
in step, resets it and opens the session again. The next call works or
reports that the device is gone.

A date a device writes without a time zone ("20261121T143000", what cameras
do) is taken as the computer's local time by the camera source.

### UsbPtpTransport

Finds the still image interface (class 6/1/1), or an MTP interface on a
vendor-specific class: one whose name contains "MTP", or one that
Microsoft's OS descriptors call MTP (asked only of vendor-specific
interfaces with exactly a bulk-in, a bulk-out and an interrupt pipe). A
device whose picture interface is in another configuration than the active
one gets that configuration set.

usb_raw's transfers have no timeout: they run in a thread of the
transport's; one that is not back in time is killed, which makes the kernel
cancel the transfer. Timeouts are 20 s for a device's answer (80 s for the
property list, 100 s for handle lists), 5 s for control requests. The
interrupt pipe (events) is not read: usb_raw runs one transfer per device at
a time, a pending interrupt read would block everything else.

`PHOTOS_PTP_TRACE=1` in the environment prints every transfer to stderr.

### CameraSource and CameraWatcher

See `src/platform/CameraSource.h`. In short:

- `CameraWatcher::Start()` reports what is connected (`"initial"` true in
  the message) and then what arrives and goes: `'camA'` / `'camR'` with
  `"key"`, `"name"`, `"kind"`. Keys: `usb:<vid>:<pid>:<serial>` (the bus
  location if the device has no serial number), `volume:<name>:<bytes>`.
- `CreateSource(key)` gives a source; `Open()` it, `ListItems()`,
  `GetThumbnail()`, `Download()`, `DeleteItem()`, `Close()`. Every call
  blocks on the device and belongs in a worker thread; calls on one source
  are serialized inside it. `LastError()` has words for the user.
- `Open()` of a phone that is locked, or that has not been allowed to show
  its pictures, fails with `B_PERMISSION_DENIED` and says what to do; it can
  be called again once the phone is unlocked. A camera without a card:
  `B_DEV_NO_MEDIA`. An unplugged device: `B_DEV_NOT_READY`.
- A PTP/MTP source lists what is in the root folders `DCIM` and `Pictures`
  (not in folders whose names begin with a dot); a device that has neither
  is listed whole. A volume source lists its `DCIM` folder. Files are
  pictures, RAW files or videos by their extension, else by the PTP format
  code.
- Thumbnails: the device's (`GetThumb`), else the EXIF thumbnail from the
  first 64 KiB of a JPEG (`GetPartialObject`, or read from the volume);
  `B_UNSUPPORTED` otherwise.
- `Download()` removes a partial file when it fails or is cancelled, and
  gives the file the item's time.

### The agent

`photos_agent` (signature `application/x-vnd.airos-Photos-Agent`, a
background application) runs a `CameraWatcher`. For an arrival that is not
"initial", when Photos is not running, it launches
`application/x-vnd.airos-Photos` with a `'camA'` message (`"key"`,
`"name"`, `"kind"`, and `"launch"` true). The message reaches Photos'
`MessageReceived()` before `ReadyToRun()`. When Photos runs, the agent does
nothing: Photos has its own watcher.

`~/config/settings/Photos/agent` with the line `open_on_connect false`
turns it off (read at every arrival).

`resources/photos_agent.launch` goes into the package as
`data/user_launch/photos_agent`; it starts `/boot/system/bin/photos_agent`
at login. launch_daemon reads these files once, when the session starts: on
the day the package is installed nothing starts the agent until the next
login, unless Photos (or the package's post-install script) launches it.

## How it was tested

Host: `make check-host` (23 tests in `tests/PtpTests.cpp`, ASan/UBSan).

In the test VM (air/OS x86_64 hrev60207+2804 under QEMU 10.2.1, xHCI), with
generated pictures (`tools/ptp-test/make-fixtures.py OUT`):

```
tools/build-cross.sh x86_64 && tools/build-cross.sh x86_64 build-x86_64/ptpls \
	&& tools/build-cross.sh x86_64 build-x86_64/fake_photos
VM_NAME=ptp tools/vm/vm-up.sh
VM_NAME=ptp tools/ptp-test/vm-test.sh OUT
```

`vm-test.sh` hot-plugs QEMU's `usb-mtp` device (a real MTP responder,
serving `OUT/phone`) and a `usb-storage` device with `OUT/card.img` (FAT32),
and runs `ptpls` and the agent against them:

- **MTP**: found, arrival and removal reported; device info and storage
  read; 20 pictures and videos listed (27 objects walked); EXIF thumbnails
  for 14; all 20 downloaded and identical (SHA-1) to the host's files,
  among them one of 23.8 MiB, names with spaces, and four whose data ends
  exactly on a packet boundary; 40 to 110 MiB/s. A download cancelled after
  4 MB returns in half a second and the device lists again. Deleting from
  the read-only device fails with "write-protected".
- **More than 4 GiB**: an object of 4,295,090,753 bytes (a sparse file)
  listed with its size and downloaded with the right SHA-1; a folder of
  3000 files listed in 5.6 s (1.8 ms per object).
- **Unplugged during downloads** (`unplug-test.sh`, 16 unplugs at random
  moments of a download): every time the download that was cut off returned
  "was disconnected" (`B_DEV_NOT_READY`) within a second or two, its
  partial file was removed and the files before it were intact. Once the
  program's team did not end afterwards: the first known limit below.
- **Memory card**: reported when mounted, listed (dates from the files'
  times), thumbnails, all copies identical; removal reported. Deleting on a
  card mounted read/write works.
- **Agent** (`agent-test.sh`, with `fake_photos` standing in for Photos):
  nothing is opened for a camera that is there when the agent starts; an
  arrival starts Photos with the message; nothing happens while Photos
  runs; `open_on_connect false` is honoured; a card that gets mounted
  starts Photos (`kind` 1); after 15 more arrivals the agent has the same
  threads and areas.

### What air/OS does with a card

A hot-plugged FAT volume is not mounted silently: mount_server asks ("The
file system on this volume is not the Be file system... Mount read-only /
Mount read/write / Cancel"). The camera arrives for Photos (and the agent)
when the user has answered and the volume is mounted, a few seconds later.
The volume source therefore does not mount anything itself.

## Not tested

No real camera, iPhone or Android phone was within reach: everything above
is QEMU's MTP responder, the made-up device of the host tests, and a FAT
image. In particular untested on hardware:

- an iPhone (PTP; the "Allow this device to access photos" question, HEIC
  files, tens of thousands of objects without `GetObjectPropList`);
- an Android phone (the MTP interface on a vendor-specific class, the
  property list in one go; some Samsung phones are known to answer it
  wrongly: the session then lists folder by folder);
- a camera (`GetThumb`, which QEMU does not have; USB 2 and full-speed
  packet sizes; cameras that list only in PTP mode);
- a transfer timeout against a device that really stops answering (the
  killed-thread path ran here only for the 400 ms waits after a cancel);
- exFAT cards (the image could not be filled without root), cards in a
  built-in SD reader.

## Known limits

- **A device unplugged between two transfers can leave a thread in the
  kernel.** usb_raw's removal hook cancels the transfers that are queued and
  only afterwards marks the device as gone; a transfer that arrives in
  between (several milliseconds: the hook waits for the controller) is
  queued and never completed, and the thread that waits for it cannot even
  be killed. A download is a row of transfers with gaps, so pulling the
  cable during one can hit this (in the tests: 1 of 16 unplugs with this
  code, 3 of 20 over all runs).
  The transport notices within a second that the device is gone, gives the
  thread up and reports "was disconnected"; Photos goes on working. But
  the team keeps that thread: when the program quits, its team stays behind
  in the kernel (`ps` shows it; no window, no Deskbar entry). The fix
  belongs in `src/add-ons/kernel/drivers/bus/usb/usb_raw.cpp` of the air/OS
  Haiku tree: mark the device as removed before cancelling, and check the
  mark under the device's lock before queueing a transfer.
- **Names that are not ASCII**: this system's FAT driver hands such a long
  name out in a form that cannot be opened (`ls` shows `Stra?e ??.jpg`,
  `stat` fails), so the file is not listed. QEMU's usb-mtp sends such names
  as "Oops" (it converts through the C locale); the initiator's own UTF-16
  handling is covered by the host tests. Cameras name their files in ASCII.
- **Events are not read** (see above): a card put into a camera, or a phone
  unlocked, while it is connected is noticed only by asking again
  (`Open()`).
- **Only DCIM and Pictures** of a phone are listed.
- A PTP session is exclusive: while a source is open the camera may show
  "connected to PC" and not take pictures. Close sources that are not used.
- `GetObjectPropList` is asked for all properties of all objects at once;
  on a phone with very many files that is a large answer (the limit is
  256 MiB).
