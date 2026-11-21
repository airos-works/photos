# The face networks

Three TensorFlow Lite files (float32) that the air/OS Neural Kit runs, on the
CPU and whole on a graphics processor (the Vulkan back end). They are made by
`tools/models/convert_faces.py` from the ONNX files their authors publish;
running it again gives these files byte for byte. `docs/FACES.md` says how
they are used and what was measured.

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `face_detector_640.tflite` | 230,108 | `1a7b74d665b82256038d7cfca7b40d4889acec39bbe0a15493d1326c398567da` |
| `face_detector_1280.tflite` | 230,108 | `8ed0e6d696d9e38454abb1c12927f34f7f8e4bb9942eb497af36b2e4108aa15d` |
| `face_embedder.tflite` | 38,581,460 | `4b45a5a806c101dd78908b7634d729b76a621b0a0198c825d60494dc2c1c3b60` |

## The detector: YuNet

- **Source:** `face_detection_yunet_2023mar.onnx` from
  [opencv_zoo](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet),
  commit `25f423d0e04c31a17254620e58febd7386da523b`, SHA-256
  `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4`.
- **Licence:** MIT, copyright (c) 2020 Shiqi Yu (`LICENSE.YuNet`).
- **Trained on:** WIDER FACE, according to its authors.
- **The two files** are the same weights for two input sizes (the network
  is convolutions only; a TensorFlow Lite file has fixed shapes).
- **Input** `input`: `[1, N, N, 3]` float32, N = 640 or 1280: blue, green,
  red, 0..255 (the bytes of a `B_RGB32` bitmap in order), the picture scaled
  into the top left, black around it.
- **Outputs,** for the strides s = 8, 16 and 32, with (N / s)² cells in row
  order: `cls_<s>` and `obj_<s>` `[1, cells, 1]` (after the sigmoid),
  `bbox_<s>` `[1, cells, 4]`, `kps_<s>` `[1, cells, 10]`. For the cell in
  column c and row r: score = sqrt(cls · obj); centre = ((c + bbox₀) · s,
  (r + bbox₁) · s); size = (exp(bbox₂) · s, exp(bbox₃) · s); landmark n =
  ((c + kps₂ₙ) · s, (r + kps₂ₙ₊₁) · s) for the right eye, left eye, nose
  tip, right and left corner of the mouth (the subject's right and left).

## The embedder: SFace

- **Source:** `face_recognition_sface_2021dec.onnx` from
  [opencv_zoo](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface),
  the same commit, SHA-256
  `0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79`.
- **Licence:** Apache License 2.0 as published by OpenCV Zoo
  (`LICENSE.SFace`). The network is from "SFace: Sigmoid-Constrained
  Hypersphere Loss for Robust Face Recognition" (Zhong, Deng, Hu, Zhao,
  Li, Liu; 2021), a MobileNet-like body with a 128 number output.
- **What is not known:** OpenCV Zoo does not say which pictures these
  weights were trained on (the SFace authors' own repository names
  CASIA-WebFace, VGGFace2 and MS1MV2, public research collections of faces
  of public figures), and two questions there about commercial use
  (opencv_zoo issues 313 and 318) have no answer. The file is redistributed
  here under the licence it is published with; whoever ships Photos
  commercially may want to look at that again.
- **Input** `input`: `[1, 112, 112, 3]` float32: (blue, green, red − 127.5)
  / 128 of the face, aligned by its five landmarks to the usual 112 × 112
  template (`kFaceTemplate` in `src/core/FaceMath.cpp`).
- **Output** `fc1`: `[1, 128]`, not normalised. Two faces are compared by
  the cosine of the angle between their vectors.

## What the conversion changes

Nothing in what the networks compute: the files agree with ONNX Runtime on
the originals to 5e-6 (detector) and 1.5e-5 (embedder) of the largest
output, and with LiteRT's reference kernels on the Neural Kit's own
interpreter. What is different, and why (all of it in the script's
comments):

- pictures are NHWC; batch normalisations are folded into the layer
  before or after; ReLUs are fused;
- PReLU (not in the Vulkan back end) is written as `a·x + relu((1 − a)·x)`,
  and YuNet's nearest-neighbour upsampling (not there either) as
  concatenations and reshapes: every operator is one the back end runs, so
  a network is one program on the graphics processor;
- SFace's own input arithmetic is left to the caller, and its first
  convolution takes blue-green-red instead of red-green-blue.

The 25 MB of the embedder that are its last layer (50,176 to 128, fully
connected) could be stored as 16 bit numbers at half the size; the Neural
Kit would then convert them on every run and keep the network on the CPU,
so they are not.
