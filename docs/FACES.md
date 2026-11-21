# Faces

Photos finds the faces in a picture and gives each one a vector of 128
numbers that lies close to the vectors of the same person's other faces.
Both steps are neural networks run by the air/OS Neural Kit, on a graphics
processor when there is one and on the CPU otherwise; what Photos makes of
the vectors (people, suggestions) is not in this document's code.

| Piece | Where |
| --- | --- |
| the networks | `models/` (`models/README.md`: sources, licences, formats) |
| making them from the published files | `tools/models/convert_faces.py` |
| everything around the networks, portable | `src/core/FaceMath.{h,cpp}`, tests in `tests/FaceTests.cpp` |
| the networks on the Neural Kit | `src/platform/FaceEngine.{h,cpp}` |
| from a shell on air/OS | `tools/face-test/facescan.cpp` (`make $(BUILD)/facescan`) |
| the same on the build host | `tools/face-test/facescan_host.cpp`, `build-host.sh` |
| measuring | `tools/face-test/evaluation/` |

## Using it

```cpp
#include "platform/FaceEngine.h"

photos::FaceEngine engine;                       // keep it: setting up is the slow part
status_t status = engine.Init("/path/to/models");   // flags: B_NEURAL_CPU_ONLY, ...
std::vector<photos::DetectedFace> faces;
status = engine.Detect(bits, width, height, bytesPerRow, faces);   // B_RGB32 layout
for (const photos::DetectedFace& face : faces) {
	// face.x, y, width, height, landmarks: fractions of the picture
	// face.score: the detector's confidence; face.quality: 0..1
	// face.embedding: 128 numbers, normalised; empty below kFaceMinQuality
}
```

- `Detect()` blocks and does one picture at a time per engine (a second
  caller waits); several engines work at once. The picture must be
  upright (apply the EXIF orientation first: a face lying on its side is
  found less well and embedded badly).
- **What to scan:** a preview of about 2000 pixels on the long side. It
  goes through the detector twice, whole (see below), which is where the
  numbers under [Evaluation](#evaluation) come from. Larger pictures work
  (above 3328 pixels they are also searched in tiles, so that faces stay as
  large for the detector), but a 24 megapixel original costs eight detector
  runs instead of two and finds little more that can be recognised.
- **Memory:** an engine takes about 150 MiB on the CPU and 115 MiB of main
  memory plus the networks' tensors on the graphics processor; the 37 MiB
  of weights are a mapped file shared by all engines. One engine per scan
  thread; one scan thread is enough on a graphics processor.
- **Setting up** takes 0.04 s on the CPU and about 1 s on a graphics
  processor (pipelines, weights, and a check of the device, below).
- `FaceEngine::kModelVersion` changes when embeddings change: store it with
  each face and search pictures again whose faces have another version.
  Vectors of different versions must not be compared.
- A device is only used if it gives the CPU's answers: `Init()` runs both
  on a test picture and keeps the CPU when they differ by more than 0.2 %
  of the largest output (a driver that computes something else would fill
  the library with wrong faces without a sound). A device that fails later
  is the Neural Kit's business: it does the run on the CPU.

### Comparing faces

Embeddings are compared by cosine similarity (`CosineSimilarity()`; the dot
product, as they are normalised). `FaceMath.h` has the thresholds, all of
them measured (below):

| Constant | Value | Meaning |
| --- | --- | --- |
| `kFaceMinScore` | 0.70 | a detection below it is not a face |
| `kFaceMinQuality` | 0.30 | below it a face has a box but no embedding |
| `kMaybeSameFaceThreshold` | 0.45 | at or above: worth asking "is this …?" |
| `kSameFaceThreshold` | 0.60 | at or above: the same person, unless another person is nearly as close |
| `kSameFaceMargin` | 0.10 | "nearly as close" |
| `kCertainFaceThreshold` | 0.70 | at or above: the same person whoever else is close |

`ClassifyFaceMatch(best, nextBest)` puts them together for the usual
question, "whose face is this, among the people I know": `best` is the
highest similarity to any face of the most similar person, `nextBest` the
highest to any face of anybody else. It answers *same* (assign without
asking), *maybe* (ask) or *no match*.

For grouping faces nobody has named yet, link two faces when their
similarity is at least `kSameFaceThreshold` and take the connected groups.
`kMaybeSameFaceThreshold` is too low for links: one pair in 170 of
different people in the same picture reaches it, and chains of such links
join people. Use it between a face and a person, or between two groups, to
ask.

## How it works

**Detector: YuNet** (2023mar; 230 kB). The picture is scaled to
fit a square input (every pixel averaged in, so that a small face in a
large picture is not sampled away) and the network gives, for every cell of
three grids, a score, a box and five landmarks. A picture goes through
whole twice: into a 640 pixel input, which finds the faces that fill the
picture (YuNet does not find faces larger than what it was trained on,
some 300 pixels), and, if it is larger than 832 pixels, into a 1280 pixel
input for the people at the back. Pictures that are still reduced to less
than 0.38 in the large input (longer than 3328 pixels) are searched in
overlapping tiles of it at half size as well; a face a tile cuts is left
to the neighbour or the whole picture. What overlaps a better detection
(intersection over union above 0.3, or mostly inside it) is dropped.

**Alignment.** The five landmarks give the rotation, scale and shift
(least squares, no shear) that put the eyes, nose and mouth where the
embedder expects them in a 112 × 112 crop; the crop is sampled from the
picture with as many samples per pixel as the face is larger than it.

**Quality** (0..1) says whether a face can be told from others: the
distance between the eyes in pixels (nothing below 8, full from 20), how
frontal it is (the nose between the eyes, the eyes apart), how sharp the
crop is (the energy of its Laplacian against its contrast) and how sure
the detector was. Faces below `kFaceMinQuality` are reported without an
embedding: they are the ones that are recognised wrongly.

**Embedder: SFace** (2021dec; a MobileNet body). 128 numbers per face,
normalised to length 1.

## Evaluation

Measured on 2026-11-21 against the owner's Immich library, read only: 728
previews (mostly 1920 × 1440; 2013 to 2026, 265 different days) picked
across the people Immich knows most pictures of, with the 2,236 faces
Immich found in them and its 193 person clusters. The pictures stayed on
the build host. Everything was run by `facescan_host`: the code of
`src/core/FaceMath.cpp` with the networks on the Neural Kit's CPU
interpreter built for Linux. `facescan` on air/OS gives the same numbers
for the same pictures (boxes, scores, qualities and embeddings of 64 faces
in 20 pictures on the CPU device in a virtual machine, and of 13 faces in
6 pictures on the GTX 1070 of the X399: equal to the last printed digit,
embeddings to five decimals).

### Detection

Of Immich's faces (found again when the boxes overlap by 0.3 or more), by
the smaller side of Immich's box in pixels of the preview:

| Score at least | All | 20–32 | 32–48 | 48–80 | 80–160 | 160 and more | Detections | not in Immich |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.60 | 99.6 % | 37/38 | 240/240 | 411/413 | 869/871 | 667/672 | 2,910 | 684 |
| **0.70** | **99.2 %** | 37/38 | 240/240 | 411/413 | 865/871 | 664/672 | 2,642 | 423 |
| 0.80 | 96.8 % | 35/38 | 228/240 | 396/413 | 848/871 | 656/672 | 2,367 | 202 |

What the detector finds and Immich does not have was looked at (contact
sheets): from 0.60 to 0.70 it is mostly not a face (hands, food, the back
of a head); above 0.70 about two thirds are faces Immich left out (looking
down, half hidden, on a poster), and the rest is the family's dog, a cat
and a few things (a backpack, a wheel, an ear). Of the 2,057 faces that
get an embedding at 0.70, about 1 % are no face and another 1 % are pets.
Hence 0.70.

How the picture is given to the detector decides whether small faces are
found (score at least 0.60; cost in runs of the 640 network, which takes
32 ms on the build host's CPU against 113 ms for 1280):

| The picture | Found | of 20–32 px | of 32–48 px | Cost |
| --- | --- | --- | --- | --- |
| whole into 640 | 96.1 % | 71 % | 92 % | 1 |
| whole into 640, tiles of 640 at half size | 98.5 % | 84 % | 98 % | 4.5 |
| whole into 640, tiles of 640 at three quarters | 99.6 % | 97 % | 100 % | 6.9 |
| whole into 1280 | 99.2 % | 97 % | 100 % | 3.5 |
| **whole into 640 and into 1280** | **99.6 %** | 97 % | 100 % | 4.5 |

### Recognition

Immich's clusters are the truth here, with two corrections made by looking
at them: four people it has in more than one cluster were joined, and ten
clusters were marked as babies and toddlers. That leaves 170 people among
the 1,821 faces with an embedding (score 0.70, quality 0.30) that Immich
has a person for. The test is the one Photos faces: a face is compared with
the faces of every person **from other days** (pictures of the same day
are near copies and flatter any method), and given the person of the most
similar one if the similarity reaches a threshold. 1,521 of the faces have
their person on another day at all.

| Nearest face at least | Assigned (of 1,521) | Right | Right, without small children |
| --- | --- | --- | --- |
| 0.45 | 95.1 % | 91.4 % | 93.7 % |
| 0.50 | 90.1 % | 94.4 % | 97.0 % |
| 0.55 | 80.9 % | 95.9 % | 98.5 % |
| 0.60 | 66.9 % | 97.8 % | 99.9 % |
| 0.65 | 54.7 % | 98.8 % | 100 % |
| 0.70 | 36.8 % | 99.6 % | 100 % |

- **`ClassifyFaceMatch()`**: *same* for 62.6 % of the recognisable faces,
  right for 99.79 % of them (2 wrong of 953, both babies); *maybe* for
  another 32.5 %, with the right person suggested 77 % of the time; no
  match for 4.9 %.
- **Different people for certain** (2,974 pairs of faces in the same
  picture): 18 pairs reach 0.45, 5 reach 0.50, 2 reach 0.55, none 0.60.
- **Babies** are where it goes wrong: 22 of the 23 wrong assignments at
  0.60 give a baby or toddler the name of a brother or sister at that age
  (similarities up to 0.73). Nothing here knows a face's age.
- **Without the quality gate** (all 1,995 faces): 97.0 % becomes 95.0 % at
  0.50 for faces of adults; the gate removes 8.7 % of the faces Immich
  has.
- **Grouping by links** (all pairs at or above a threshold): at 0.60, 85 %
  of the faces are in 164 groups of two or more, the rest single; one
  group holds more than one person (the babies of one family, about a
  hundred faces), none mixes adults; the six people with the most faces
  are in 6, 3, 2, 8, 1 and 5 groups each (years apart, with and without
  glasses), which is what naming and the *maybe* threshold are for. At
  0.70: 212 groups, 73 % of the faces, 99.7 % in the right group.

### Speed

Per picture of 1920 × 1440 (two detector runs) with *n* embedded faces;
the sample has 2.8 per picture.

| Machine | Device | Detector 640 + 1280 | Embedder, per face | Picture with 2 faces | Set-up |
| --- | --- | --- | --- | --- | --- |
| X399 workstation (the kit uses 16 threads) | CPU | 36 + 92 ms | 41 ms | 242 ms | 0.04 s |
| X399 | GTX 1070, Vulkan (NVK) | 8.5 + 32.5 ms | 11 ms | 85 ms | 0.97 s |
| build host (Xeon E5-2650 v4, 16 threads) | CPU, host build | 32 + 113 ms | 42 ms | | |

On the graphics processor every operator of all three networks runs on the
device, in one program each (`neural info`: 86 of 86, 86 of 86, 116 of
116), and `neural compare` agrees with the CPU to 1e-6 (detectors) and
7e-6 (embedder) of the largest output. About 20 ms per picture are not the
networks: scaling the picture into the two inputs and warping the faces.

A camera-sized picture (5760 × 4320, a preview with 23 faces enlarged
three times): eight detector runs, 1.4 s on the build host's CPU, 22 of
the 23 faces found again and four more.

### Not measured

- The desktop's Radeon (RADV) and any other Vulkan driver than NVK; the
  Neural Kit's own tests pass on RADV, and `FaceEngine` checks a device
  against the CPU before using it.
- arm64.
- Pictures that are not upright, and faces of people who are not in this
  one family's library: the thresholds come from 170 people, most of them
  European adults and their children.

## Limits

- Brothers and sisters as babies are told apart badly (above).
- Dogs and cats get faces and vectors now and then. They group with
  themselves.
- Faces in profile, half hidden or smaller than about 20 pixels between
  the eyes in what is scanned get no embedding.
- The embedder is 37 MiB, most of it its last layer.

## Doing it again

```sh
python3 -m venv venv && venv/bin/pip install numpy onnx onnxruntime flatbuffers ai-edge-litert pillow
venv/bin/python tools/models/convert_faces.py --check      # models/*.tflite from opencv_zoo
tools/face-test/build-host.sh                              # build-host/face-test/facescan_host
export IMMICH_URL=http://server:2283 IMMICH_TOKEN=... FACES_WORK=/somewhere/private
venv/bin/python tools/face-test/evaluation/fetch_sample.py # reads only
venv/bin/python tools/face-test/evaluation/run.py final --min-score 0.6 --min-quality 0
venv/bin/python tools/face-test/evaluation/detection.py final
venv/bin/python tools/face-test/evaluation/recognition.py final --quality 0.3
```

On air/OS: `facescan --models DIR [--cpu] [--bench] [--compare] [--dump DIR]
[--json] PICTURE...`; `neural info|compare|bench models/face_embedder.tflite`.
