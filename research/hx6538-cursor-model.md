# HX6538 (WiseEye2) for On-Device Cursor Detection — Research Report

**Date:** 2026-09-23
**Question:** Can we run a custom neural model on the Watcher's Himax HX6538 to detect the mouse cursor (and UI elements) directly on-device, instead of doing image differencing on the host?

**Verdict: Yes, feasible.** The HX6538 already runs YOLOv8n/YOLO11n object detection at 192×192 in Himax's own model zoo. A single-class cursor detector is a *smaller* problem than COCO person detection, and we can generate unlimited synthetic training data (render cursor sprites over our own screenshots). The main work is the training/export/flash pipeline, not the hardware.

---

## 1. HX6538 (WiseEye2) hardware specs

| Item | Spec |
|---|---|
| Big CPU | Arm Cortex-M55 @ 400 MHz |
| Little CPU | Arm Cortex-M55 @ 150 MHz |
| NPU | Arm Ethos-U55 microNPU @ 400 MHz (~50 GOPs per Himax) |
| SRAM | 2 MB system SRAM + 512 KB TCM (up to 2432 KB configurable) |
| Boot ROM | 64 KB |
| External flash | 100 MHz QSPI, max 16 MB (128 Mb) |
| Power | ~1.2 mW average for TFLM person-detect @ 96×96 (Himax slide); deep power-down 1 µA |
| Sensor I/F | CSI (the Watcher uses its Himax camera over SSCMA/SPI to the ESP32-S3) |

Claimed vs WiseEye1 (HX6537): 32× inference speed, 50× energy efficiency. The dual-M55 + Ethos-U55 combo is the same NPU family used in many tinyML vision products, so the toolchain is mature.

**Sources:** CNX Software (Grove Vision AI V2 teardown), Seeed product PDF, Himax WiseEye2 slide deck (manuals.plus PDF).

## 2. ML software stack

- **TensorFlow Lite Micro (TFLM)** — the primary runtime. Himax's model zoo entries are all `tflm_*`.
- **SSCMA-Micro** — Seeed's cross-platform inference framework that runs *on* the HX6538: handles image capture, preprocessing, NN invoke, AT-command result reporting. This is what the Watcher's Himax firmware speaks (our `INVOKE`/`snaptest` goes through it).
- **Arm Vela compiler** — takes a quantized `.tflite` and compiles/schedules it for the Ethos-U55 (operator partitioning NPU vs CPU fallback). **Mandatory step**: the deployable artifact is `*_int8_vela.tflite`. Plain `int8.tflite` is rejected by the device ("Invoke failed").
- **ExecuTorch** — PyTorch path exists (`torch_mb_cls` MobileNetV2 in the zoo), but TFLM is the well-trodden road.
- **Edge Impulse** — officially supports Grove Vision AI V2 (WiseEye2) as a target board.
- **CMSIS-NN / CMSIS-DSP / CMSIS-CV** — available for CPU-fallback kernels on the M55 (Helium vector extensions).

**Sources:** HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2 README + model_zoo; mark-mdo47/expts_grove_vision_ai_v2; Seeed wiki.

## 3. YOLO support — what actually runs

From Himax's own model zoo (`HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2/tree/main/model_zoo`):

| Model | File | Input |
|---|---|---|
| YOLOv8n object detection | `yolov8n_od_192_delete_transpose_0xB7B000.tflite` | 192×192 |
| YOLO11n object detection | `tflm_yolo11_od/` | 192×192 (same family) |
| YOLOv8n pose | `tflm_yolov8_pose/` | — |
| Face detection, MobileNet cls, KWS | — | — |

Notable: the YOLOv8n file is named `..._delete_transpose_...` — Himax **removed transpose ops from the export** because the Ethos-U55/Vela flow handles them poorly. Expect to do the same (export tweak, not a blocker).

SenseCraft AI (Seeed's no-code platform) lists **YOLO v5 & v8**, MobileNet V1/V2, EfficientNet-Lite as deployable, and supports **custom model upload** (model file + label list).

**Throughput anchors (measured/claimed, not extrapolated):**
- Seeed's Modbus RS485 Vision AI Camera (same HX6538): **AI recognition at 480×480, 10 FPS** (product spec).
- Official training notebook's INT8 eval of Swift-YOLO tiny @192: **128 FPS — but that is on the Colab T4 GPU during evaluation, NOT on-device.** Do not quote it as device FPS.
- No published on-device FPS number for YOLOv8n@192 on the HX6538 was found.

**Estimate for YOLOv8n @ 192×192 on HX6538 (reasoned, not measured):** YOLOv8n is 8.7 GFLOPs @640 (Ultralytics). FLOPs scale with input area: (192/640)² ≈ 0.09 → **~0.78 GFLOPs @192**. At ~50 GOPs peak NPU throughput and typical 30–50% sustained efficiency on real conv nets, that's roughly **15–30 FPS** including M55-side NMS post-processing. Single-class (nc=1) trims the head a little further. This is an estimate — profiling on hardware is needed.

## 4. Model constraints

1. **Quantization:** full INT8 quantization required, then **Vela compile for Ethos-U55** → `*_int8_vela.tflite`. This is the only accepted format.
2. **Input shape:** square, and Seeed recommends **≤ 240×240** (192×192 is the validated default). Non-square inputs are rejected ("Invoke failed").
3. **Size budget:** 16 MB QSPI flash shared between firmware and model; the YOLOv8n@192 model lives at flash offset `0xB7B000`. YOLOv8n INT8 ≈ 3.2 MB weights — fits comfortably. SRAM (2 MB) must hold TFLM arena + activations; Himax ships YOLOv8n@192, so that envelope is proven.
4. **Ops:** stick to Vela-friendly ops (conv, depthwise-conv, pooling, resize, add/mul). Avoid transposes/reshapes in the head (cf. `_delete_transpose`).

Our 416×416 captures exceed the recommended input — plan is to **downscale to 192×192 (or 224×224)** on the Himax before inference (SSCMA does resize/crop in its preprocessing).

## 5. How to train a custom model — three working paths

### Path A — Official: Roboflow + Colab `sscma.train` (recommended start)
Documented end-to-end in the Seeed wiki ("Deploying Models from Datasets to Grove Vision AI V2"):
1. Label data in **Roboflow** (or download a Universe dataset).
2. Open Seeed's **Google Colab notebook** (T4 GPU), train with `sscma.train configs/swift_yolo/swift_yolo_tiny_1xb16_300e_coco.py` — set `num_classes`, `height=width=192`, `epochs` 50–100, `data_root`, `load_from` pretrained checkpoint (transfer learning).
3. Export → produces `*_int8_vela.tflite` (plus other formats — **only the vela one works on-device**).
4. **SenseCraft Model Assistant → Upload Custom AI Model** (name + model file + labels) → "Send Model" flashes it (~3–5 min) and shows live preview with overlays.

**Swift-YOLO tiny** is Seeed's house detector for this chip — smaller/faster than YOLOv8n, designed for exactly this envelope.

### Path B — Ultralytics + Vela (most flexible)
From mark-mdo47's experiments: train **YOLOv5/YOLOv8** in Ultralytics → export ONNX → **convert ONNX → int8_vela.tflite** (quantize to INT8, run Arm Vela targeting Ethos-U55 config) → deploy via SenseCraft Model Assistant. Good if we want the Ultralytics training ecosystem (augmentation, hyperparameter search).

### Path C — Edge Impulse / SenseCraft quick-train (fastest to first result)
- **Edge Impulse** supports the WiseEye2 board target directly (impulse design → deployment).
- **SenseCraft AI "Quick Generate"** uses **YOLO-World** to synthesize a single-class model from a text prompt + a handful of captured images ("Capture to Train": ≥10 photos). Closest thing to one-shot: point the camera at cursors, capture, confirm, train, deploy — no code.

## 6. Cursor-detector feasibility

**Problem difficulty:** easy by object-detection standards. One class ("cursor"), high-contrast small rigid sprite (arrow, I-beam, hand), on a structured background (our screenshots). This is far simpler than COCO-80 person detection.

**Dataset strategy (our unfair advantage):** fully synthetic + real validation.
- Render OS cursor sprites (arrow, I-beam, hand, crosshair — macOS/Windows/Linux sets) at random positions/scales/rotations over **our own real 416×416 captures** (we have hundreds in `himax-captures/`). Programmatic bounding boxes = free labels, thousands of images in minutes.
- Augment: brightness/contrast jitter, JPEG noise, slight blur (matches camera softness), glare spots (John flagged glare as a real issue).
- Validate on held-out **real** captures with the physical cursor visible.

**Model options, smallest-first:**
| Option | Params (INT8) | Input | Est. FPS | Notes |
|---|---|---|---|---|
| Tiny custom CNN + regression head (heatmap or bbox) | 50–200 KB | 192×192 gray | 30+ | Hand-designed; most work, fastest |
| Swift-YOLO tiny, nc=1 | ~1 MB | 192×192 | 20–40 | Official flow, proven on chip |
| YOLOv8n, nc=1 | ~3.2 MB | 192×192 | 15–30 | Heaviest; model-zoo proven |

**Grayscale note:** our frames are effectively grayscale JPEGs. Options: replicate to 3 channels (zero-effort, standard), or train a 1-channel variant (saves ~3× on the first conv only — negligible). Replicate channels.

**Resolution note:** at 192×192, a 16×24 px cursor in the 416×416 frame becomes ~7×11 px — small but detectable (YOLO's P3 head handles ~8 px objects; Swift-YOLO is tuned for this). If small-object recall suffers, 224×224 input is within the ≤240 budget and buys ~36% more pixels.

**Cursor-state handling:** train one class with all states (arrow/I-beam/hand) as positives — John's earlier note that the cursor changes shape is handled naturally by a detector, unlike our template/differencing code.

**Power:** always-on inference in the low milliwatts; the NPU gates per inference. Compared to streaming every JPEG over MQTT, on-device detection *reduces* system power and bandwidth.

## 7. Deploying to the Watcher (practical path)

The Watcher's HX6538 runs Himax SSCMA firmware today; our ESP32-S3 sends `INVOKE` and gets JPEG chunks back. With a cursor model flashed:

1. **Flash the model** into the Himax's model flash slot (e.g. `0xB7B000` region, per `common_config.h` in the zoo examples). Routes:
   - **Easy:** SenseCraft Model Assistant over USB — needs the Watcher's USB port exposed and John's hands-on help (fits his existing "you propose, I click" pattern).
   - **Hard:** OTA model update through our MQTT → ESP32-S3 → Himax SPI path. Would need S3 firmware support for writing the Himax flash via SSCMA protocol. Not currently implemented — real firmware project.
2. **S3 firmware change:** instead of (or in addition to) `snaptest` JPEG streaming, send the detect `INVOKE` and parse the returned **bounding boxes** (SSCMA AT protocol already defines detection result frames — see `SSCMA-Micro/docs/protocol/at_protocol.md`). The S3 then forwards `{"t":"cursor","x":..,"y":..,"conf":..}` over MQTT. Our host pipeline consumes coordinates directly — no more differencing.
3. **Fallback:** keep JPEG streaming as a debug/verification path (visual confirmation of what the model sees).

**Caveat:** the Watcher's Himax firmware is Himax's SSCMA build, not the Grove V2 Arduino firmware — but the model-slot mechanism (TFLM + Vela model in flash, invoked via AT commands) is the same architecture. Confirm the Watcher's SSCMA firmware version supports model invoke with bounding-box output before committing to training.

## 8. Examples in the wild

- **HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2** — official examples: YOLOv8n OD, YOLO11n OD, pose, face-mesh, KWS. Build/flash/run docs for Linux/macOS/Windows. (129 stars, 74 forks)
- **mark-mdo47/expts_grove_vision_ai_v2** — community: Ultralytics → ONNX → int8_vela.tflite → SenseCraft deploy, then face-following over UART. Proves the custom-model path works outside Seeed's notebooks.
- **Seeed_Arduino_SSCMA / SSCMA-Micro** — the middleware our Watcher already speaks; custom models appear as the same `invoke()` API returning classes/boxes.
- **SenseCraft AI** — no-code: pretrained YOLOv5/v8, MobileNet, EfficientNet-Lite, plus custom upload and YOLO-World quick-train.

## 9. Recommended next steps

1. **Confirm the Watcher's SSCMA firmware supports model invoke w/ bbox output** (check firmware version + AT protocol docs; test with a zoo model if a dev Grove V2 board is handy — $16).
2. **Buy/borrow a Grove Vision AI V2** as a dev board: iterate on model training without risking the Watcher, then port the proven `.tflite` over.
3. **Build the synthetic cursor dataset** (script: sprite renderer over `himax-captures/*.jpg` → YOLO-format labels; ~5k images).
4. **Train Swift-YOLO tiny nc=1 @192** via Path A (fastest proven route), evaluate mAP on real captures.
5. **Profile on-device FPS** and small-cursor recall; bump to 224×224 if needed.
6. **Plan the model-flash route** with John (USB SenseCraft session vs. MQTT OTA firmware project).

## 10. Sources

- HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2 (GitHub) — specs, model_zoo, build/flash docs: https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2
- Seeed wiki: "Deploying Models from Datasets to Grove Vision AI V2" (Roboflow → Colab → int8_vela → SenseCraft): https://wiki.seeedstudio.com/grove_vision_ai_v2_sscma/
- Seeed wiki: Grove Vision AI Module V2 overview: https://wiki.seeedstudio.com/grove_vision_ai_v2/
- mark-mdo47/expts_grove_vision_ai_v2 (custom YOLOv5 → Vela → deploy): https://github.com/mark-mdo47/expts_grove_vision_ai_v2
- CNX Software: Grove Vision AI V2 (HX6538 specs): https://www.cnx-software.com/2024/01/19/16-grove-vision-ai-v2-module-features-wiseeye2-hx6538-arm-cortex-m55-ethos-u55-ai-microcontroller/
- Ultralytics YOLOv8 docs (8.7 GFLOPs @640 for v8n): https://github.com/ultralytics/ultralytics/blob/HEAD/docs/en/models/yolov8.md
- SSCMA-Micro AT protocol: https://github.com/Seeed-Studio/SSCMA-Micro/blob/1.0.x/docs/protocol/at_protocol.md
- Edge Impulse docs: Grove Vision AI V2 (WiseEye2) board support: https://docs.edgeimpulse.com/hardware/boards/seeed-grove-vision-ai-module-v2-wise-eye-2
