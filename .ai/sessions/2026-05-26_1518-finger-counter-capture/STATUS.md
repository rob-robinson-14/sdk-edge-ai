# STATUS

## Done
- Scaffolded `applications/finger_counter_capture` for `nrf54lm20dk/nrf54lm20b/cpuapp`.
- Re-uses the existing `arducam_mega` driver + DT binding from `applications/person_detection` (no driver duplication).
- Logs routed to RTT; `uart20` (J-Link VCOM) reserved for binary frames at 1 Mbaud.
- Button 1 (`sw0`) triggers a single 128x128 RGB565 capture, sent as a 16-byte header + 32 KiB payload.
- Host tool `tools/capture_host.py` (pyserial + numpy + Pillow) demuxes frames and writes per-label PNGs.
- README documents build, flash, capture, and Edge Impulse upload workflow.
- **Build verified clean** on this checkout:
  - First attempt failed with `DT_N_*_arducam_mega_0_P_spi_interframe_delay_ns undeclared`. Root cause: `list(APPEND DTS_ROOT ...)` was after `find_package(Zephyr)`, so Zephyr's DTS pass never picked up the `arducam,mega.yaml` binding and the node was generated without the `spi-device.yaml` properties (31 macros vs. 63 in `person_detection`'s build).
  - Fixed by moving the `DTS_ROOT` append before `find_package(Zephyr)`. Rebuild from scratch passes: 72352 B flash / 53000 B RAM.

## Verified on hardware (2026-05-26 17:01)
- DK detected after user powered it on: J-Link SN `001051824273`, PCA10184.
- Flash + RTT boot verification: OK.
- Identified `uart20` => `/dev/ttyACM1` (vcom 1) on this DK empirically.
- Initial single-frame-per-press build produced unrecognisable images
  ("lines of colour") because the very first frame after a cold
  `video_stream_start` is underexposed (Arducam AE/AGC not yet converged).
- Diagnostic dumps confirmed: raw payload bytes go from a brief gradient
  into a flat `0x0840` pattern — signature of an underexposed first frame.
- Multi-frame test (5 frames per press) showed sensor stats progressed
  correctly across frames, and all 5 looked like real photos of the user's
  hand.
- Final fix: capture `AE_WARMUP_FRAMES + 1` (default 5) frames per press,
  send only the last one. `capture_host.py --count 3` now produces 3 PNGs
  with full dynamic range and channel means in the 70..90 range; user
  confirms they look like real photos.

## Diagnostic tooling shipped
- `tools/diagnose_offline.py` produces ~15 decode variants from a raw
  header+payload dump. README's Troubleshooting section explains how to
  use it if a future hardware change re-surfaces a decode issue.

## Not yet done (deliberate; out of scope of this turn)
- Inference firmware (the `finger_counter` app that consumes the EI Axon NPU model archive).
- No on-target self-test; sample.yaml runs CI build-only.
