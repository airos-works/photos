# Photos: notes for people and agents working on this repository

Photos is the air/OS photo library application (github airos-works/photos,
branch `main`). Native C++17 on the Be API, plus one Rust static library that
puts LightCraft's RAW decoder behind a C interface.

## Layout

| Path | What | Rules |
| --- | --- | --- |
| `src/core` | library database, import, EXIF, edits, face matching, JSON, hashing | portable C++: no Be headers, tested on the build host |
| `src/sync` | Immich client and the two-way sync engine, over `core/Http.h` | portable |
| `src/ptp` | PTP/MTP initiator over an abstract transport | portable |
| `src/net` | libcurl implementation of `Http` | portable, needs libcurl |
| `src/platform` | Be API without windows: decoders, thumbnails, Neural Kit faces, USB transport, device watching, the PhotoCraft bridge | |
| `src/ui` | the application | |
| `src/agent` | `photos_agent`: launches Photos when a camera, phone or card arrives | |
| `src/tool` | `photos_tool`: the library from a shell (tests, scripting) | |
| `raw/` | Rust crate `photos-raw` (staticlib): LightCraft's `lightcraft-raw` and pipeline behind `raw/include/photos_raw.h` | |
| `models/` | the face detection and embedding networks (TensorFlow Lite) and their licences | |
| `tests/` | host tests (`make check-host`), `tests/integration` against `tools/fake-immich.py` | |
| `tools/` | cross build, packaging, the test VM, fixtures | |

## Building and testing

- `tools/build-cross.sh x86_64|arm64` cross-compiles with the air/OS SDK
  (`/data1/airos/sdk/<arch>`, dependencies in `/data1/airos/deps/<arch>`).
  `SKIP_RAW=1` leaves the Rust library out.
- `make check-host` runs the portable tests with ASan/UBSan.
- `tools/test-sync.sh` runs the sync engine against `tools/fake-immich.py`.
- `tools/vm/vm-up.sh` boots an air/OS test VM; `tools/vm/deploy.sh` copies a
  build into it, `tools/vm/app.sh` starts it, `tools/vm/hey.sh` scripts it
  (docs/TESTING.md).
- The package: `tools/package.sh x86_64|arm64` after a cross build makes
  `artifacts/photos-<version>-<arch>.hpkg` by hand. The build server makes the
  same with airos-ci (`build_photos` in `apps/build-app-packages.sh`), on
  every push to `main` (`.github/workflows/airos.yml`; CI then pushes a
  "[skip ci]" commit with the README's build links: `git pull --rebase`
  before pushing). Keep the two recipes in step.

## Conventions

- Haiku coding style: tabs, `fMember`, `_PrivateMethod()`, return type on its
  own line in definitions. Namespace `photos` for portable code.
- Sizes in the UI are multiples of the plain font's size (HiDPI); colours
  come from `src/ui/Theme` (the system's light/dark switch).
- Originals are never modified: edits are parameters in the database plus a
  rendered version file.
- No credentials, tokens or personal pictures in the repository. Test
  pictures are generated (`tools/make-fixtures.py`) or public-domain.
- **Never run a build that is signed in to a real Immich server without
  `PHOTOS_NO_UPLOAD=1`** (a test library's pictures would be uploaded to it).
  Uploads and deletions are tested against `tools/fake-immich.py`.
- Commit as you go; `main` is the integration branch. Bump `version` in
  `resources/Photos.PackageInfo` for every package handed out.
