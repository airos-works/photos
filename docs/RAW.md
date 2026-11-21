# RAW files (and HEIC) in Photos

Photos develops camera RAW files with LightCraft's engine. The engine is
Rust; Photos is C++. `raw/` is the bridge: a small Rust crate, `photos-raw`,
that depends on LightCraft's crates at one pinned commit and exports the C
interface of `raw/include/photos_raw.h`. It is built as a static library
(`libphotos_raw.a`) and linked into `Photos` and `photos_tool`.

```c
#include "photos_raw.h"

photos_raw_image image;
char error[256];
if (photos_raw_develop_file(path, 2560, &image, error, sizeof(error)) == PHOTOS_RAW_OK) {
	// image.pixels: BGRA (a B_RGB32 bitmap's rows), image.stride bytes per row
	photos_raw_free_image(&image);
}
```

| Function | Does | Cost (X100T, 16 MP X-Trans; see [Timings](#timings)) |
| --- | --- | --- |
| `photos_raw_is_raw`, `photos_raw_is_raw_file` | recognises a RAW container from its first bytes | microseconds |
| `photos_raw_info_file` | size, orientation, capture time, camera, lens, exposure, GPS: headers only | milliseconds (it reads the file) |
| `photos_raw_preview_file`, `_ex` | the camera's embedded JPEG, as stored, and how to turn it upright | milliseconds |
| `photos_raw_preview_image_file` | the embedded preview decoded, upright, scaled | tens of milliseconds |
| `photos_raw_develop_file` | LightCraft's rendering of the sensor data | 0.3 s at 512 px to several seconds full size |
| `photos_heif_info_file`, `photos_heif_decode_file` | iPhone HEIC pictures | about a second for 12 MP |

All of them are safe to call from several threads at once; errors are
negative codes plus a message; a damaged file never takes the program down
(a panic inside the decoder is caught at the interface and comes back as
`PHOTOS_RAW_INTERNAL`). Pictures come back as BGRA with alpha 255, sRGB,
already upright; they are freed with `photos_raw_free_image`.

The work runs on the library's own thread pool
(`photos_raw_set_threads(n)` before the first call, or `PHOTOS_RAW_THREADS`;
by default three quarters of the CPUs, at most 16). The calling thread only
waits, so it may be any thread, whatever its stack: a window's, a
`spawn_thread` worker with Haiku's 256 KiB. A call cannot be cancelled.

## What "LightCraft's default rendering" is

`photos_raw_develop_file` returns the picture LightCraft shows for a file
nobody has edited, and exports from it untouched:

1. `lightcraft_engine::files::load_bytes`: decode the sensor data, fit the
   camera's look from the file's own JPEG (or take the DNG's profile),
   rebuild clipped highlights, apply the as-shot white balance and the
   colour model, bin the mosaic for small sizes, turn the picture upright.
2. The settings import gives a new photo
   (`lightcraft_catalog::Photo::camera_defaults`, repeated in
   `default_settings` in `raw/src/lib.rs`): as-shot white balance, capture
   sharpening 40, colour noise reduction 25, the file's embedded lens
   corrections on.
3. `lightcraft_pipeline::render`, the CPU pipeline, to 8-bit sRGB.

`raw/tests/parity.rs` holds this to LightCraft itself: it imports the same
file into a LightCraft session, exports it with no edits (what
`lightcraft-cli render` does) and compares the two pictures. On the samples
(Nikon NEF, Apple DNG, Canon CR2, Fujifilm X-Trans RAF) they are equal bit
for bit at full size.

What differs from the LightCraft application:

- **The GPU.** LightCraft renders on the GPU when it can and on the CPU
  otherwise; the two are built to agree, and Photos always uses the CPU
  pipeline. (wgpu is compiled into the library because `lightcraft-engine`
  depends on it, and dropped by the final link.)
- **Sizes below full.** For a preview LightCraft bins the sensor data and
  renders at the size of its loupe; Photos asks for `max_edge` the same way,
  so a reduced picture is LightCraft's preview of that size, not its export
  scaled down. Noise reduction and sharpening are sized for the original
  either way (`SourceInfo::native_long`).
- **User defaults.** A default preset set in LightCraft's own settings is
  LightCraft's; Photos renders the built-in defaults.
- **The camera's JPEG is not the target.** For most cameras the fitted look
  lands close to the embedded JPEG (the Fujifilm samples do); for some it is
  visibly different (the Canon EOS 40D sample comes out darker and warmer
  than its JPEG). That is LightCraft's rendering of that file, not an error
  of the bridge.

## Formats

From LightCraft's own documentation (`crates/raw/src/lib.rs`,
`docs/raw-parity.md` at the pinned commit):

- **Decoded from sensor data:** DNG (uncompressed, lossless and lossy JPEG,
  Deflate, JPEG XL; CFA and linear, so phone and ProRAW files too), Canon
  CR2, CR3 (lossless and most C-RAW) and CRW, Nikon NEF and NRW, Sony ARW
  (uncompressed, ARW2, lossless), Fujifilm RAF (Bayer and X-Trans,
  uncompressed, lossless and lossy compressed), Panasonic RW2 and Leica RWL,
  Pentax PEF, Olympus ORF (uncompressed variants), Samsung SRW, Hasselblad
  3FR/FFF, Minolta MRW, Epson ERF, Mamiya MEF, Kodak DCS.
- **Recognised, shown from the embedded JPEG only:** variants the decoder
  refuses (compressed ORF, some CR3 configurations, Sigma X3F, other
  TIFF-based raws). `photos_raw_develop_file` returns
  `PHOTOS_RAW_UNSUPPORTED` with the reason; `photos_raw_info_file` and the
  preview functions still work, so Photos shows the camera's JPEG.
- LightCraft counts 557 camera models verified on a real file and estimates
  that about 94% of photographers' cameras decode from sensor data.

`photos_raw_is_raw` says "this is a RAW container", not "this will develop".

## HEIC

iPhone pictures are HEIF containers with HEVC inside. LightCraft has a
pure-Rust decoder for them (`lightcraft-heif` on `heic-rs`), which needs no
system library; Photos gets it through `photos_heif_*` when the crate is
built with its `heif` feature, which is on by default. It honours the
container's rotation and the picture's colour profile (Display P3 becomes
sRGB). On two iPhone 12 mini pictures the result matches the preview an
Immich server (libheif) made of them.

LightCraft's own releases leave this decoder out because HEVC is covered by
patents in some countries, which is for whoever distributes the program to
decide. Building without it: `PHOTOS_RAW_FEATURES="" tools/build-raw.sh ARCH`;
`photos_heif_available()` is then 0 and the functions return
`PHOTOS_RAW_NOT_BUILT`.

## Timings

Measured 2026-11-21 with `rawtest` in the air/OS test VM (hrev60207+2804,
16 virtual CPUs under KVM on a Xeon E5-2650 v4, the library's default pool of
12 threads). The host was busy with other builds at the time (load 40 to 55
on 48 threads), so an idle machine does better and single figures move by a
half between runs; they are for telling 0.1 s from 1 s from 10 s. Seconds,
and the process's peak memory:

| File | `max_edge` 512 | 2048 | full size |
| --- | --- | --- | --- |
| Fujifilm X100T RAF, 16 MP X-Trans | 0.36 s, 88 MB (512 × 341) | 0.63 s, 140 MB (1632 × 1088) | 3.4 s, 1.14 GB (4896 × 3264) |
| Fujifilm X-T20 RAF, 24 MP X-Trans, compressed | 1.1 s, 113 MB | 1.6 s, 212 MB (2000 × 1333) | 6.2 s, 1.71 GB |
| Nikon D70 NEF, 6 MP | 0.46 s, 32 MB | 1.8 s, 274 MB | 2.4 s, 0.50 GB |
| Canon EOS 40D CR2, 10 MP | 0.65 s, 50 MB | 1.1 s, 191 MB (1944 × 1296) | 2.9 s, 0.81 GB |
| Apple iPhone 6s Plus DNG, 12 MP | 1.2 s, 338 MB | 1.8 s, 336 MB | 4.1 s, 0.96 GB |
| Sony RX100 ARW, 20 MP | 0.45 s, 84 MB | 1.1 s, 215 MB | 4.8 s, 1.53 GB |

- The embedded preview decoded to 512 px (`photos_raw_preview_image_file`):
  0.06 to 0.14 s, 15 to 45 MB. The embedded JPEG itself
  (`photos_raw_preview_file`): 0.01 to 0.1 s (it is 0.1 to 0.9 MB; the time
  is reading the RAW file). `photos_raw_info_file`: 0.03 to 0.09 s.
- HEIC, iPhone 12 mini, 12 MP: 0.8 to 1.1 s and 260 MB at full size; a
  smaller `max_edge` costs the same (the picture is decoded whole, then
  scaled).
- Four 1024 px developments of the X100T file at once: 1.2 s for all four,
  287 MB. Three full-size developments of the D70 file at once: 4.3 s,
  1.26 GB.
- The pool's size matters more on a busy machine than on an idle one: in the
  same VM, with as many threads as virtual CPUs (16), the 512 px X100T
  development took 0.7 to 3.6 s instead of 0.3 to 0.5 s with 4 to 12. Hence
  the default of three quarters of the CPUs, at most 16
  (`photos_raw_set_threads`, `PHOTOS_RAW_THREADS`).

The pictures are the same bytes on air/OS and on Linux (compared for the
X100T files, the 40D file and a HEIC), and a development into a `BBitmap`
saved through the Translation Kit looks as it should.

What follows from them:

- **Thumbnails and the first view come from the embedded JPEG.**
  `photos_raw_preview_image_file(path, 512, …)` is the fast path; a
  development is for when the picture is looked at.
- **Cache developments.** A screen-size development costs about a second and
  a full-size one several; neither should happen twice for the same file.
- **One full-size development at a time.** A full-size development holds
  about 70 to 85 bytes per pixel while it runs (1.1 GB for 16 MP, 1.7 GB for
  24 MP: float pictures of the whole sensor); several at once add up.
  Reduced sizes are cheap (30 to 340 MB) and run well side by side on the
  one thread pool they share.
- **A requested size is an upper bound.** Reduced sizes bin the mosaic by a
  whole factor: the result keeps at least 90% of `max_edge`, or 75% for
  X-Trans sensors, which can only bin by three (2048 asked of a 4896 px
  X100T file gives 1632). Ask for a quarter more than is needed when the
  size matters, or for full size.

## Building

`tools/build-raw.sh ARCH` (`x86_64`, `arm64`) builds the library for air/OS;
`tools/build-cross.sh` calls it first. It needs the Rust toolchain airos-ci
installs (`sdk/build-rust.sh`: `$AIROS_TOOLCHAIN/rust`, a pinned nightly with
`rust-src`) and the air/OS SDK:

- `tools/raw-env.sh` sets up the cross build: the SDK's gcc as linker and C
  compiler (`tools/raw-cc`, `tools/raw-c++`), std built from source for the
  Haiku target (`-Zbuild-std=std,panic_abort`), and for arm64 the target
  specification in `raw/targets` (a copy of airos-ci's). The registry and
  git caches go to a cargo home of our own when the toolchain's belongs to
  the CI runner (`PHOTOS_CARGO_HOME`; default
  `/mnt/HaikuWork/toolchains/rust/cargo`, else `raw/target/cargo-home`).
- Cargo's output stays in `raw/target` (`PHOTOS_RAW_TARGET_DIR` moves it).
  The script copies the library to `build-ARCH/libphotos_raw.a` and writes
  what the final link needs after it to `build-ARCH/libphotos_raw.libs`.
- The first build compiles LightCraft's engine and about 330 crates
  (about 60 CPU-minutes: 11 minutes on the 48-thread build host while it was busy, 2 to 3 for a host build on a quieter one); a run with nothing changed takes about a second.
- The network is needed once, for crates.io and the two git repositories
  (`airos-works/lightcraft`, `storytold/craft-libs`); `PHOTOS_RAW_OFFLINE=1`
  after that.

The link line:

```
g++ … objects… build-ARCH/libphotos_raw.a \
    -Wl,--gc-sections -Wl,--exclude-libs,libphotos_raw.a -Wl,-z,noexecstack \
    -lgnu -lbsd -lgcc_s -lroot -lnetwork
```

The first three options matter. The archive is LightCraft's whole engine
(165 MB), of which the link keeps what the interface reaches: 10 MB of code
in `rawtest`, a 13 MB program. `--gc-sections` drops the rest, and
`--exclude-libs` lets it: a Haiku executable exports every global symbol,
which would make each function of the archive something to keep (52 MB of
code without it). The archive's file name must stay `libphotos_raw.a` for
that option to find it. `-z noexecstack` is what rustc passes for its own
programs; without it ld warns about the SDK's `crtn.o`.

Rust's panics unwind with the same unwinder as C++ exceptions (the SDK's
shared libgcc); both work in one program (`rawtest selftest`), and the
library links beside libbe and the rest of Photos' libraries without
clashes.

On the build host: `tools/raw-host.sh test --release` runs the crate's tests
(`tools/raw-test/fetch-samples.sh` downloads the CC0 samples they use;
without them the file tests are skipped), and `rawtest` builds against the
host library the same way.

### Moving to a newer LightCraft

Every `rev` in `raw/Cargo.toml` is the same commit of
`airos-works/lightcraft` (dependencies and the three wgpu patches). Change
them together, run `tools/raw-host.sh update -p lightcraft-engine` (or copy
LightCraft's `Cargo.lock` over `raw/Cargo.lock` first to take its versions of
everything else, as was done for the first one), then
`tools/raw-host.sh test --release`: the parity test says whether
`default_settings` still matches LightCraft's import defaults. Regenerate
`raw/NOTICE` (`tools/raw-notice.py`).

## rawtest

`make … build-ARCH/rawtest` (or `tools/build-cross.sh ARCH build-ARCH/rawtest`)
builds `tools/raw-test/rawtest.cpp`, linked as Photos is:

```
rawtest version
rawtest info FILE
rawtest preview FILE [MIN_EDGE] [OUT.jpg]
rawtest previewimage FILE [MAX_EDGE] [OUT.ppm]
rawtest develop FILE [MAX_EDGE] [OUT.ppm]
rawtest heifinfo FILE
rawtest heif FILE [MAX_EDGE] [OUT.ppm]
rawtest threads FILE [MAX_EDGE] [N]
rawtest selftest
```

`tools/raw-test/compare.py OUT.png A B` puts two pictures side by side (the
camera's JPEG and a development) for looking at.

## Limits

- 8 bits per channel, sRGB. The pipeline can give 16-bit and wide-gamut
  output; the interface does not ask for it yet.
- A development cannot be cancelled once started.
- No edits: the interface renders LightCraft's defaults. Photos' own edits
  work on the developed picture.
- The whole file is read into memory (RAW files are tens of megabytes).
- Running out of memory ends the program (Rust's allocation failure is not
  an error that comes back): do not develop several large files at full
  size at once on a small machine.
- A file cut short still has its header: `photos_raw_info_file` then
  answers with the embedded preview's size, as it does for a variant the
  decoder cannot read, and the development fails with `PHOTOS_RAW_DAMAGED`.
- arm64: the library and `rawtest` build and link for `aarch64-unknown-haiku`
  (2026-11-21); nothing has run on an arm64 machine yet.
- Licences: LightCraft is MIT OR Apache-2.0; `raw/NOTICE` lists every crate
  the library is built from. All are permissive (MIT, Apache-2.0, BSD, ISC,
  Zlib and the like).
