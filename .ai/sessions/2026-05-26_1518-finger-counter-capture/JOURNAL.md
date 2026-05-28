# JOURNAL

## 2026-05-26 15:18 — Initial scaffold

Decisions:
- Single-USB-cable workflow: logs to RTT, binary frames over uart20 (J-Link VCOM).
  Avoids requiring a second USB-UART adapter and keeps the data stream byte-clean.
- Re-use `applications/person_detection`'s `arducam_mega` driver and DT binding via
  `DTS_ROOT` + relative `target_sources`. Keeps the driver in one place.
- Wire format kept deliberately minimal (16-byte header, no CRC, no escaping).
  RGB565 is unambiguous and 1 Mbaud is reliable over the J-Link VCOM; a corrupted
  frame is cheap to discard or re-capture.
- Button-edge triggered via gpio interrupt giving a semaphore; main loop owns the
  video pipeline and the 32 KiB static `frame_buf` so we never block in IRQ.

Inputs surveyed:
- `applications/person_detection/{src/main.c, src/drivers/arducam_mega.c, boards/*.overlay, prj.conf, CMakeLists.txt}` — capture loop, DT binding, dependencies.
- `samples/edge_impulse/data_forwarder/{app.overlay, src/main.c}` — two-UART pattern (rejected in favour of RTT for simpler cabling).
- `samples/edge_impulse/hello_ei/{boards/*.conf, prj.conf}` — Axon-EI integration shape (referenced in README "Next step" section).
- `zephyr/boards/nordic/nrf54lm20dk/nrf54lm20dk_common.dtsi` — confirmed `sw0`/`led0`/`led1` aliases and `uart20` defaults.

Next session:
- Branch `samples/edge_impulse/hello_ei` into `applications/finger_counter` once the user has the EI Axon NPU model zip; port the `capture_one_frame` plumbing in, feed pixels into the EI `signal_t` as packed `0xRRGGBB` floats.

## 2026-05-26 16:10 — Build fix + flash blocked on hardware

Build error on first compile:
- `DT_N_S_soc_S_peripheral_50000000_S_spi_c8000_S_arducam_mega_0_P_spi_interframe_delay_ns undeclared`.
- Diagnosis: counted DT macros for the arducam node in both builds —
  - person_detection build: 63 macros
  - finger_counter_capture build: 31 macros
  The new app's build was missing every property contributed by `spi-device.yaml`.
- Root cause: `list(APPEND DTS_ROOT ...)` was placed after `find_package(Zephyr)`.
  Zephyr resolves DTS bindings during `find_package`, so a later `DTS_ROOT`
  append is silently ignored. `person_detection` doesn't hit this because its
  bindings live under its own `APPLICATION_SOURCE_DIR/dts`, which Zephyr
  auto-includes regardless of `DTS_ROOT`.
- Fix: move `list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../person_detection)`
  before `find_package(Zephyr ...)` in CMakeLists.txt.
- Clean rebuild: PASS (72352 B flash, 53000 B RAM).

Flash blocked:
- User confirmed DK is plugged in with Arducam wired, but `nrfutil device list`
  returns 0, `lsusb` shows no Segger USB ID, `/dev/ttyACM*` is empty.
  Polled 5x over 15s — still nothing. Reported back to user to verify cable
  / hub / DK power before retrying.

## 2026-05-27 09:46 — finger_counter brought up end-to-end

User dropped in their EI zip (FOMO model `finger_test_1`) and hit a wall.
Multi-step bring-up journey, captured here because every step was a
useful lesson about the EI + Axon stack on this DK:

1. **BUILD_ASSERT in main.c was wrong**.
   `EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME` is *always* 1 for image
   impulses (it means "channels per sample"); the per-pixel count lives
   in `EI_CLASSIFIER_RAW_SAMPLE_COUNT`. Updated the assert.

2. **log_top_class() assumed image classification**, but the user
   trained FOMO. Split `log_result()` into two `#if
   EI_CLASSIFIER_OBJECT_DETECTION == 1` paths: bounding boxes for FOMO,
   `classification[]` for image classification. Same source now serves
   either impulse type.

3. **"Out of memory, can't allocate matrix_ptrs[0]"** at the entry to
   `run_classifier`. EI's DSP block needs to allocate
   `1 * EI_CLASSIFIER_NN_INPUT_FRAME_SIZE * sizeof(float)` = 192 KiB
   for a 128x128 RGB model. The kernel heap (`CONFIG_HEAP_MEM_POOL_SIZE`)
   is the wrong place: EI's `ei_malloc` in
   `modules/edge-impulse-sdk-zephyr/edge-impulse-sdk/porting/zephyr/ei_classifier_porting.cpp`
   wraps **picolibc** `malloc`, which uses
   `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE`. Default is `-1` (all remaining
   RAM). Removed our 200 KiB kernel-heap allocation; that 200 KiB
   reverted to picolibc's arena automatically.

4. **Axon interlayer buffer was 64 KiB over-allocated**. The
   `conf_overlay.conf` from EI shipped `262144` (256 KiB) but the model
   header declared `MAX_IL_BUFFER_USED = 196608` (192 KiB). Trimmed to
   the actual value, freeing 64 KiB of BSS. That alone made the 200 KiB
   picolibc arena fit.

5. **Usage fault — unaligned memory access** in `run_nn_inference`.
   Tracked through `addr2line` to `nordic_axon.h:159` (the int8 scale
   loop). Reading the surrounding code revealed a **bug in the EI
   Nordic Axon wrapper**: `vector_size = height * width` ignores
   `channel_cnt`. For the user's RGB model (`channel_cnt = 3`) the loop
   writes 3x past the end of the stack VLA. Patched
   `modules/edge-impulse-sdk-zephyr/edge-impulse-sdk/classifier/inferencing_engines/nordic_axon.h`
   to multiply by `channel_cnt`. The patch lives in the local NCS
   checkout, NOT in the FetchContent extraction — that copy is unused
   (the module's include path comes first). `west update` will revert
   the patch.

6. **Stack overflow** after the patch. The now-correctly-sized 48 KiB
   int8 VLA didn't fit in our 24 KiB main stack. Bumped
   `CONFIG_MAIN_STACK_SIZE` to 65536.

End result: working live FOMO inference at ~22 Hz (DSP 25.6 ms +
classification 20.9 ms). The user's model is undertrained — it returns
all 4 trained classes every frame at confidence 0.996 in the same
bounding box — but that's a training-data problem, not a deployment
problem.

Recommendations documented in `applications/finger_counter/README.rst`:
- Strongly prefer **image classification** over FOMO for the
  "how-many-fingers" question; one impulse output is exactly the
  semantics the user asked for. (We left the source able to handle both
  via the `#if EI_CLASSIFIER_OBJECT_DETECTION` split.)
- If they keep FOMO, train as **grayscale** to dodge the SDK bug and
  cut RAM by ~3x.

Worth filing upstream as an EI bug; for now the patch is local.

## 2026-05-26 17:08 — Continuous capture + inference app scaffold

User asked for: photo count guidance, continuous capture (no button), explicit
EI upload steps, and the deployment / inference application.

Changes:
- `finger_counter_capture/src/main.c` no longer waits on a button. After
  initialising and discarding `AE_WARMUP_FRAMES` (4) at boot, the loop
  captures + sends frames continuously. LED0 toggles per frame.
  Measured throughput: 1.92 fps over 10 frames (UART-bound at 1 Mbaud).
- `README.rst` updated: removed button-press instructions, added the
  per-class `--count 100` workflow with a bash loop iterating all 6
  classes.
- New app `applications/finger_counter/` (deployment / inference):
  - CMakeLists.txt cribbed from `samples/edge_impulse/hello_ei/CMakeLists.txt`
    plus the arducam driver source from `../person_detection`. DTS_ROOT
    is appended BEFORE `find_package(Zephyr)` (lesson from earlier).
  - `prj.conf` enables `CONFIG_EDGE_IMPULSE_SDK=y`, `CONFIG_NRF_AXON=y`,
    the Zephyr video stack, and console-on-uart20 (LOG_BACKEND_UART
    is on by default; we removed the RTT-only override since we don't
    stream binary frames in this app).
  - `Kconfig` exposes `CONFIG_EDGE_IMPULSE_PATH` (default
    "finger_counter.zip") and `CONFIG_FINGER_COUNTER_INFERENCE_INTERVAL_MS`.
  - `src/main.c` mirrors the capture loop from `finger_counter_capture`,
    converts RGB565 -> the EI image input format (one float per pixel,
    packed `0xRRGGBB`), and calls `run_classifier()`. Verified packing
    against `edge-impulse-sdk/classifier/ei_run_dsp.h::extract_image_features`
    lines 1172-1178.
  - The model resolution is taken from
    `EI_CLASSIFIER_INPUT_WIDTH`/`_HEIGHT` so the same source code works
    for both 96x96 and 128x128 trained models (the only two resolutions
    the Arducam Mega supports below 320 px).
- App will not build until the user trains a model in Edge Impulse
  Studio and drops the zip in. Documented clearly in README +
  prj.conf comments. Drop-in path goes through
  `west build -- -DEXTRA_CONF_FILE=<unzipped>/conf_overlay.conf`.

## 2026-05-26 17:01 — AE warm-up fix

User reported the first round of captured PNGs (single frame per press) looked
like "lines of colour" instead of a hand. Empirical investigation:

1. Captured one raw 32784-byte stream and decoded it as 15 variants
   (rgb565 BE/LE, bgr565 BE/LE, yuv422 yuyv/uyvy, rotations, flips,
   96x96, rgb888, grayscale, header-skip variants). User confirmed the
   original `rgb565_be` decoder was already the closest match — i.e. the
   pixel format, byte order, R/B order, dimensions, and rotation were all
   correct. So the decode logic was not the bug.
2. The raw payload's first 60 bytes showed a slow gradient
   (`a5 96 → be 38`), then a few noisy bytes, then the tail filled with
   `08 40 08 40 ...` (near-black with a green tint). Classic AE-not-yet-
   converged underexposure.
3. Patched firmware to grab 5 frames per press and tag each with its
   frame index. Means / stds confirmed the sensor is still progressing
   between frames (R mean 68 -> 81 -> 77 -> 76 -> 76 across frames 0..4),
   AE settled by frame ~2. User confirmed all 5 looked like recognisable
   images of the hand.
4. Decided that `person_detection` doesn't hit this because it captures
   continuously every 500 ms, so AE is permanently converged. Our app
   only streams on demand, so every button press is effectively a cold
   start.
5. Final policy: `AE_WARMUP_FRAMES = 4`. Each button press streams 5
   frames internally but emits only the last one. Adds ~120 ms latency,
   which is invisible to a human pressing a button.

Final verification: `capture_host.py --count 3` produces 3 PNGs with full
dynamic range (R/G/B min=0, max=255), means in the 70-90 range, stds
around 80-90. User confirms all 3 PNGs look like real photos of the hand.

## 2026-05-26 16:38 — DK powered on, full bring-up verified

User reported "DK was turned off". After power-on: J-Link `001051824273`,
PCA10184, two VCOMs at `/dev/ttyACM0` and `/dev/ttyACM1`. Flash + verify
sequence ran clean:

- `nrfutil device program ... --options chip_erase_mode=ERASE_ALL` => success.
- RTT log via `JLinkRTTLogger -Device nRF54LM20A_M33 -If SWD -Speed 4000
  -USB 001051824273 /tmp/rtt.log`:
    *** Booting nRF Connect SDK v3.3.0-ba167d9f3db4 ***
    *** Using Zephyr OS v4.3.99-fd9204a02d52 ***
    <inf> capture: Finger capture ready. Press Button 1 to capture a frame.
- Mapped uart20 -> /dev/ttyACM1 via passive dual-port byte counter during
  button-press window. No DT chosen / docs lookup needed; empirical.
- End-to-end capture: 3 PNGs from `capture_host.py --label two --count 3`.
  Each 128x128 RGB, full dynamic range, mean ~125, std ~65 per channel
  (i.e. real scene content, not all-black / saturated / uniform-noise).

Gotcha caught:
- First run of `capture_host.py` was wrapped in `timeout 35` and produced
  empty stdout because SIGTERM truncated the Python stdout buffer. Re-ran
  with `python3 -u` and `> /tmp/capture.log` to keep output even when
  the timeout fires.
