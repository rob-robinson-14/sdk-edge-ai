#!/usr/bin/env python3
# Copyright (c) 2026 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
"""Host-side receiver for finger_counter_capture firmware.

Reads framed RGB565 image payloads from the DK's J-Link VCOM and writes one
PNG per frame into ``<out>/<label>/<timestamp>_<seq>.png``. After collecting
a class folder you can upload it to Edge Impulse Studio with::

    edge-impulse-uploader --category split --label two images/two/*.png

Wire format produced by the firmware (little-endian fields):

    offset  size  field
    0       4     magic    = AA 55 AA 55
    4       2     width
    6       2     height
    8       1     pixfmt   (0x01 = RGB565, camera native big-endian per pixel)
    9       3     reserved
    12      4     length   (bytes in payload)
    16      N     payload
"""
from __future__ import annotations

import argparse
import datetime as _dt
import struct
import sys
from pathlib import Path

import numpy as np
import serial
from PIL import Image

MAGIC = b"\xAA\x55\xAA\x55"
HEADER_FMT = "<4sHHB3sI"  # magic, w, h, pixfmt, reserved, length
HEADER_LEN = struct.calcsize(HEADER_FMT)  # 16
PIXFMT_RGB565 = 0x01


def _read_exact(ser: serial.Serial, n: int) -> bytes:
    """Read exactly n bytes from `ser`, blocking until they arrive."""
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            continue  # timeout; keep trying
        buf.extend(chunk)
    return bytes(buf)


def _sync_to_magic(ser: serial.Serial) -> None:
    """Scan the stream until we see the 4-byte magic."""
    window = bytearray()
    while True:
        b = ser.read(1)
        if not b:
            continue
        window += b
        if len(window) > 4:
            del window[0]
        if bytes(window) == MAGIC:
            return


def _decode_rgb565_be(payload: bytes, w: int, h: int) -> np.ndarray:
    """Decode camera-native big-endian RGB565 -> HxWx3 uint8 numpy array."""
    if len(payload) != w * h * 2:
        raise ValueError(
            f"payload size {len(payload)} != expected {w * h * 2}"
        )

    # Two bytes per pixel, big-endian: pixel = (hi << 8) | lo
    raw = np.frombuffer(payload, dtype=np.uint8).reshape(h, w, 2)
    pix = (raw[:, :, 0].astype(np.uint16) << 8) | raw[:, :, 1].astype(np.uint16)

    r5 = (pix >> 11) & 0x1F
    g6 = (pix >> 5) & 0x3F
    b5 = pix & 0x1F

    # 5/6-bit -> 8-bit via bit replication (no float division needed).
    r8 = ((r5 << 3) | (r5 >> 2)).astype(np.uint8)
    g8 = ((g6 << 2) | (g6 >> 4)).astype(np.uint8)
    b8 = ((b5 << 3) | (b5 >> 2)).astype(np.uint8)

    return np.stack([r8, g8, b8], axis=-1)


def _read_one_frame(ser: serial.Serial) -> tuple[int, int, bytes]:
    _sync_to_magic(ser)

    # We just consumed the 4 magic bytes; the rest of the header is 12 bytes.
    rest = _read_exact(ser, HEADER_LEN - 4)
    header = MAGIC + rest
    _magic, w, h, pixfmt, _reserved, length = struct.unpack(HEADER_FMT, header)

    if pixfmt != PIXFMT_RGB565:
        raise ValueError(f"unsupported pixfmt 0x{pixfmt:02x}")
    if length != w * h * 2:
        raise ValueError(
            f"header inconsistency: length={length} but {w}x{h} RGB565 "
            f"would be {w * h * 2}"
        )

    payload = _read_exact(ser, length)
    return w, h, payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial device, e.g. /dev/ttyACM1")
    parser.add_argument("--baud", type=int, default=1_000_000,
                        help="must match the firmware (default 1000000)")
    parser.add_argument("--label", required=True,
                        help="class label, e.g. zero, one, two, three, four, five")
    parser.add_argument("--out", default="images", type=Path,
                        help="output root directory (default: images/)")
    parser.add_argument("--count", type=int, default=0,
                        help="stop after N frames (0 = unlimited, Ctrl-C to stop)")
    args = parser.parse_args()

    label_dir = args.out / args.label
    label_dir.mkdir(parents=True, exist_ok=True)

    print(f"Opening {args.port} @ {args.baud} baud")
    print(f"Saving images into {label_dir}/")
    print("Press Button 1 on the DK to capture; Ctrl-C to stop.\n")

    with serial.Serial(args.port, args.baud, timeout=0.5) as ser:
        ser.reset_input_buffer()
        seq = 0
        try:
            while True:
                w, h, payload = _read_one_frame(ser)
                rgb = _decode_rgb565_be(payload, w, h)
                ts = _dt.datetime.now().strftime("%Y%m%d_%H%M%S_%f")
                out_path = label_dir / f"{ts}_{seq:05d}.png"
                Image.fromarray(rgb, mode="RGB").save(out_path)
                seq += 1
                print(f"[{seq:5d}] saved {out_path} ({w}x{h})")
                if args.count and seq >= args.count:
                    print(f"Reached --count {args.count}, exiting.")
                    return 0
        except KeyboardInterrupt:
            print(f"\nStopped after {seq} frames in {label_dir}/.")
            return 0


if __name__ == "__main__":
    sys.exit(main())
