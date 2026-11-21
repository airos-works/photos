# How Photos is built

```
 src/ui        the window: sidebar, header, grid, viewer, editor, people, import
    │  messages (platform/Messages.h, ui/Ui.h)
 src/platform  Session ── Pictures (tiles, viewer pictures, decoding)
    │                 ├── Importer        files, cameras
    │                 ├── FaceScanner     Neural Kit (FaceEngine)
    │                 ├── PhotoCraftBridge  working files, node monitor
    │                 ├── CameraWatcher / CameraSource  USB PTP/MTP, DCIM volumes
    │                 └── sync thread
 src/core      Library (SQLite) · People · Edit · Image · Exif · Settings   portable,
 src/sync      ImmichClient · SyncEngine                                     tested on
 src/ptp       PTP/MTP initiator                                             the host
 raw/          LightCraft's RAW and HEIF decoders behind photos_raw.h (Rust)
```

## The library

A directory (`~/Pictures/Photos Library`):

```
library.db                       SQLite: photos, versions, imports, people, faces, settings
originals/YYYY/MM/<name>         as imported, never modified
versions/<photo>/<version>.jpg   edits rendered by Photos, saves from PhotoCraft
thumbs/<xx>/<photo>-<rev>.jpg    512 px thumbnails (JPEG, or WebP as Immich sent it)
previews/<xx>/<photo>-<rev>.jpg  developed RAW files, Immich's screen-size previews
faces/<xx>/<face>.jpg            a small picture of every face found
edits/<photo>/                   working files handed to PhotoCraft
```

- A photo's **revision** grows whenever its look changes; thumbnails, previews
  and the tiles in memory are keyed by it, so nothing stale is ever shown and
  old files are simply pruned.
- A photo's capture time is **local wall time** (what the camera's clock
  showed), so days and months are those of where the picture was taken.
- A photo may have no original on this computer (`path` empty): it is on Immich
  and shown from the server's thumbnail and preview until the original is
  fetched.

## Versions and edits

The picture shown for a photo is its *current version*: the original, or a row
of `versions`.

- An **edit** (made in Photos) is a set of numbers (`core/Edit.h`: turn, flip,
  straighten, crop, light, colour) plus the picture rendered from its *base*.
  Editing an edited photo changes the same numbers and renders again from the
  same base: nothing is ever rendered twice in a row.
- An **external** version is a file another program saved (PhotoCraft). An edit
  made afterwards has that version as its base.
- The editor works on a 2048 px copy; a thread renders the newest values and
  skips the ones in between. *Done* renders at full size on the session's
  worker thread.

## PhotoCraft

`PhotoCraftBridge` copies the photo to `edits/<photo>/` (JPEG and PNG as they
are, anything else developed to PNG), launches PhotoCraft with it and watches
the folder with the node monitor. A file that has stopped changing and whose
checksum Photos has not seen becomes a version. For a layered document the
composite is taken (the merged image of a PSD, `composite/full.png` or
`composite/preview.png` of a `.pcraft`), and the document itself is what
PhotoCraft gets next time. Saves made while Photos was not running are picked
up at its next start.

## Pictures

- **Tiles** of the grids are square bitmaps of exactly the size drawn, made by
  a pool of threads from the thumbnail files and kept in a memory budget
  (least recently used first out). The newest request is served first: that is
  what the user is looking at.
- **Decoding**: JPEG with libjpeg-turbo (reduced-size decoding for
  thumbnails), WebP with libwebp, RAW and HEIF with the RAW library,
  everything else with the one translator that reads the format. The
  Translation Kit's roster is never asked to *identify* a file: that hands it
  to every translator, and a damaged file has crashed one inside the program.
- **RAW**: thumbnails and the first view come from the camera's embedded
  preview; the development (LightCraft's default rendering) is cached as a
  JPEG in `previews/`.

## People

`FaceEngine` (docs/FACES.md) gives, for a picture, boxes, a quality and a
128-number embedding per face. `core/People` keeps all embeddings in memory:

- a new face that is surely a named person (similarity ≥ 0.60 with a margin of
  0.10 to anyone else, or ≥ 0.70) is assigned; a person is in a photo once;
- otherwise it joins the group of unnamed faces whose middle it is nearest to
  (≥ 0.60), never a group that already has a face of the same photo;
- naming a group, confirming or rejecting a suggestion adds examples and looks
  through the unassigned faces again.

## Cameras

`CameraWatcher` reports PTP/MTP devices (USB still-image interfaces, MTP by
its descriptors) and mounted volumes with a `DCIM` folder. `photos_agent`, a
background application started with the desktop, runs one and launches Photos
with the camera's key when something arrives. In Photos the import view opens
the device, lists it on a thread, fetches thumbnails, and hands the chosen
items to the `Importer`. The session stays open while the device's pictures are
shown: PTP handles are only good for one session.

"New" is decided before anything is downloaded: an item whose name, size and
time was imported before (even if the photo was deleted since), or whose name
and size match a photo in the library, is not new. After the download the
checksum decides.
