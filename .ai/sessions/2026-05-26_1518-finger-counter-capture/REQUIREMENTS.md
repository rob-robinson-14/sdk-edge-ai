# Requirements — finger_counter_capture

## Goal
Provide a capture firmware + host tool to gather labelled image data of a
hand holding up 0..5 fingers, then upload that data to Edge Impulse Studio
for training a 6-class image classification model that will later be
deployed to the nRF54LM20DK Axon NPU.

## Hardware
- nRF54LM20DK (board target: `nrf54lm20dk/nrf54lm20b/cpuapp`)
- Arducam Mega 5MP B401 on SPIM22 (same pins as `applications/person_detection`)
  - SCK=P1.4, MISO=P1.5, MOSI=P1.6, CS=P1.7
- 3V3 supplied to camera via Board Configurator (VDD:IO = 3.3 V)

## Firmware behaviour
- On boot: init camera @ RGB565 128x128, init LEDs, init Button 1 (sw0)
- Stream logs to RTT (no UART logs) so the J-Link VCOM stays binary-clean
- On every Button 1 press:
  - LED0 ON during capture, OFF after
  - LED1 blink on successful send
  - Send one image frame over `uart20` using the framing protocol below
- Frame format (16 B header + payload):
  - 4 B magic   = `0xAA 0x55 0xAA 0x55`
  - 2 B width   (LE)
  - 2 B height  (LE)
  - 1 B pixfmt  (0x01 = RGB565, byte order = camera native big-endian)
  - 3 B reserved (0)
  - 4 B length  (LE, bytes of payload)
  - N B payload (width * height * 2 bytes RGB565)

## Host tool (`tools/capture_host.py`)
- Reads frames continuously from a serial port
- Decodes RGB565 -> RGB888 via numpy
- Saves each frame as PNG into `out_dir/<label>/<timestamp>.png`
- CLI: `python capture_host.py --port /dev/ttyACM1 --label two --out images/`
- Optionally re-syncs by re-scanning for magic if a header CRC mismatch occurs
- Prints a running count per label

## Non-goals
- No on-device classification (separate `finger_counter` app, later)
- No JPEG / YUV format (RGB565 only — matches the deployment path)
- No web UI; uploads via `edge-impulse-uploader` CLI after collection
