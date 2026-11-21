# Photos

Photos is the photo library application of [air/OS](https://airos.works), in the
spirit of iPhoto and Google Photos: all your pictures in one place, by time and
by people, with the basic edits, import from cameras and phones, and two-way
sync with an [Immich](https://immich.app) server.

It is a native application (C++ on the Be API). Faces are found and recognised
on the machine itself with the air/OS Neural Kit; camera RAW files are developed
by [LightCraft](https://github.com/airos-works/lightcraft)'s engine; bigger
edits go to [PhotoCraft](https://github.com/airos-works/photocraft) and come
back as versions.

<!-- airos-ci:latest-builds:start -->
## Latest builds

Built automatically by air/OS CI from commit `eddc49d` on 2026-11-21 ([all files](https://github.com/airos-works/photos/releases/tag/latest)).

| Architecture | Package |
|---|---|
| arm64 | [photos-1.0.0-2-arm64.hpkg](https://github.com/airos-works/photos/releases/download/latest/photos-1.0.0-2-arm64.hpkg) |
| x86_64 | [photos-1.0.0-2-x86_64.hpkg](https://github.com/airos-works/photos/releases/download/latest/photos-1.0.0-2-x86_64.hpkg) |

Install with `pkgman install <file>`, or copy the file into `/boot/system/packages`. Haiku SDK: x86_64 hrev60207-2821-g02fa9ead2d, arm64 hrev60207-2821-g02fa9ead2d.
<!-- airos-ci:latest-builds:end -->

## What it does

- **Your photos by time.** *Photos* is the timeline, newest first, as **Years**,
  **Months**, **Days** or **All Photos**; years and months show a selection of
  their pictures and lead into the next level. *Recents* has what was added
  last, *Favorites* what you gave a heart, *Last Import* the latest import,
  *Recently Deleted* the trash (30 days). The grid stays fast with tens of
  thousands of photos: only what is on screen is loaded.
- **One photo.** Double-click (or Enter) opens it; the wheel zooms, a double
  click switches between fitted and actual size, the arrow keys go on. *Info*
  shows when and with what it was taken, where the original is, its versions
  and the people in it.
- **People.** Photos looks for faces in the background (Neural Kit: on the GPU
  or NPU when there is one, else the CPU; nothing leaves the computer). Faces
  that look alike are grouped under *Unnamed faces*; click a group and say who
  it is. From then on new photos of that person are recognised, and "Is this
  Anna too?" asks about the faces it is not sure of: every answer teaches it.
  A face can also be named on its photo (*Faces* in the viewer).
- **Edit.** Crop (free or to a shape), straighten, turn, flip; exposure,
  contrast, highlights, shadows; saturation, warmth, tint; *Enhance* picks
  values for you. Edits never touch the original: they are numbers kept with
  the photo, the sliders show them again next time, and *Revert to original*
  is always there.
- **Edit in PhotoCraft.** Hands the photo to PhotoCraft. Every save there
  appears in Photos as a new version of the photo, by itself. A layered
  document (PSD, .pcraft) is kept, so the next *Edit in PhotoCraft* goes on
  where you stopped.
- **JPEG, PNG, HEIC, TIFF, WebP, AVIF and camera RAW** (DNG, Canon CR2/CR3,
  Nikon NEF, Sony ARW, Fujifilm RAF incl. X-Trans, Panasonic RW2, Pentax PEF,
  Olympus ORF: what LightCraft reads). RAW files show the camera's own preview
  at once and then LightCraft's development.
- **Cameras, phones, memory cards.** Connect one and Photos opens (also when
  it was not running) and shows its pictures: **Import new** takes only what
  is not in the library yet, or choose pictures, or import all. Cameras and
  phones are read over PTP/MTP; cards and cameras that appear as a disk through
  their `DCIM` folder.
- **Immich, both ways.** Sign in under *Settings* and the photos on the server
  appear in the library (their thumbnails at once, an original when you open,
  edit or export its photo, or all of them if you ask for that); photos that
  are only here are uploaded. Favourites go both ways. See
  [docs/IMMICH.md](docs/IMMICH.md) for exactly what is and is not synced.

## Where things are

| | |
| --- | --- |
| The library | `~/Pictures/Photos Library` (originals, versions, thumbnails, `library.db`) |
| Settings | `~/config/settings/Photos/settings.json` |
| Face models | `data/Photos/models` of the package |
| The agent that opens Photos for a camera | `bin/photos_agent`, started by `data/user_launch/photos_agent` |

`photos_tool` drives the same library from a shell (`photos_tool` without
arguments lists its commands), and `hey Photos …` scripts the running
application (`hey Photos getsuites` lists the commands).

## Building

Cross-built on Linux with the air/OS SDK (airos-ci):

```
tools/build-cross.sh x86_64     # or arm64; also builds the RAW library (Rust)
make check-host                  # the portable tests (ASan/UBSan)
tools/test-sync.sh               # sync against tools/fake-immich.py
```

The package is made by airos-ci (`apps/build-app-packages.sh … photos`).
[AGENTS.md](AGENTS.md) has the layout and conventions,
[docs/DESIGN.md](docs/DESIGN.md) how it is built, and
[docs/TESTING.md](docs/TESTING.md) how it is tested.

## Things to know

- **Videos** are not part of the library: they are skipped on import and when
  syncing.
- **HEIC** is decoded by LightCraft's own decoder, compiled in by default
  (`PHOTOS_RAW_FEATURES="" tools/build-raw.sh` builds without it).
- The **face models** are YuNet (MIT) and SFace (Apache-2.0) from the OpenCV
  Zoo, converted for the Neural Kit: see [models/README.md](models/README.md)
  and [docs/FACES.md](docs/FACES.md).
- A real camera or phone has not been connected to a development machine yet:
  PTP/MTP was tested against QEMU's MTP device and a FAT card
  ([docs/DEVICES.md](docs/DEVICES.md)).

## Licence

MIT (see [LICENSE](LICENSE)). The RAW library embeds LightCraft (MIT or
Apache-2.0) and its dependencies: [raw/NOTICE](raw/NOTICE).
