#!/usr/bin/env python3
"""Offline variant: read a previously captured raw stream (header + payload)
and emit many decode variants as PNGs. No serial needed.
"""
import io, struct, sys
from pathlib import Path
import numpy as np
from PIL import Image

raw_path = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/diag_raw.bin")
out_dir = Path(sys.argv[2] if len(sys.argv) > 2 else "/tmp/finger_diag")
out_dir.mkdir(parents=True, exist_ok=True)

data = raw_path.read_bytes()
magic = data.find(b"\xAA\x55\xAA\x55")
if magic < 0:
    print("no magic found in stream"); sys.exit(1)
hdr = data[magic : magic + 16]
_m, w, h, pixfmt, _r, length = struct.unpack("<4sHHB3sI", hdr)
print(f"header: w={w} h={h} pixfmt=0x{pixfmt:02x} length={length}")
payload = data[magic + 16 : magic + 16 + length]
print(f"payload bytes={len(payload)} expected={w*h*2}")
print(f"first 96 bytes hex: {' '.join(f'{b:02x}' for b in payload[:96])}")


def dec565(p, w, h, endian, swap_rb):
    raw = np.frombuffer(p, dtype=np.uint8).reshape(h, w, 2)
    if endian == "be":
        pix = (raw[..., 0].astype(np.uint16) << 8) | raw[..., 1].astype(np.uint16)
    else:
        pix = (raw[..., 1].astype(np.uint16) << 8) | raw[..., 0].astype(np.uint16)
    r5 = (pix >> 11) & 0x1F
    g6 = (pix >> 5) & 0x3F
    b5 = pix & 0x1F
    r = ((r5 << 3) | (r5 >> 2)).astype(np.uint8)
    g = ((g6 << 2) | (g6 >> 4)).astype(np.uint8)
    b = ((b5 << 3) | (b5 >> 2)).astype(np.uint8)
    if swap_rb:
        return np.stack([b, g, r], axis=-1)
    return np.stack([r, g, b], axis=-1)


def dec_yuv(p, w, h, order):
    raw = np.frombuffer(p, dtype=np.uint8).reshape(h, w, 2)
    if order == "yuyv":
        y = raw[..., 0].astype(np.float32); uv = raw[..., 1].astype(np.float32)
    else:
        y = raw[..., 1].astype(np.float32); uv = raw[..., 0].astype(np.float32)
    u = uv.copy(); v = uv.copy()
    u[:, 1::2] = u[:, 0::2]; v[:, 0::2] = v[:, 1::2]
    c = y - 16.0; d = u - 128.0; e = v - 128.0
    r = np.clip(1.164*c + 1.596*e, 0, 255)
    g = np.clip(1.164*c - 0.392*d - 0.813*e, 0, 255)
    b = np.clip(1.164*c + 2.017*d, 0, 255)
    return np.stack([r, g, b], axis=-1).astype(np.uint8)


variants = {}
need = w * h * 2

# Pixel format variants on the raw payload
for end in ("be", "le"):
    for sw, name in ((False, "rgb565"), (True, "bgr565")):
        try: variants[f"{name}_{end}"] = dec565(payload[:need], w, h, end, sw)
        except Exception as e: print(f"skip {name}_{end}: {e}")
for o in ("yuyv", "uyvy"):
    try: variants[f"yuv422_{o}"] = dec_yuv(payload[:need], w, h, o)
    except Exception as e: print(f"skip yuv422_{o}: {e}")

# Header-skip (in case there are preamble bytes)
for skip in (1, 2, 4, 8, 16, 32, 64):
    if len(payload) >= need + skip:
        try: variants[f"rgb565_be_skip{skip}"] = dec565(payload[skip:skip+need], w, h, "be", False)
        except Exception as e: print(f"skip skip{skip}: {e}")

# 96x96 fallback
if len(payload) >= 96*96*2:
    try: variants["rgb565_be_96x96"] = dec565(payload[:96*96*2], 96, 96, "be", False)
    except Exception as e: print(f"skip 96x96: {e}")

# JPEG
if payload[:2] == b"\xff\xd8":
    eoi = payload.find(b"\xff\xd9")
    if eoi > 0:
        try: variants["jpeg_decoded"] = Image.open(io.BytesIO(payload[:eoi+2])).convert("RGB")
        except Exception as e: print(f"skip jpeg: {e}")

# Rotations/flips of baseline
if "rgb565_be" in variants:
    b = variants["rgb565_be"]
    variants["rgb565_be_rot90cw"] = np.rot90(b, k=-1)
    variants["rgb565_be_rot180"]  = np.rot90(b, k=2)
    variants["rgb565_be_rot90ccw"] = np.rot90(b, k=1)
    variants["rgb565_be_hflip"]   = np.fliplr(b)
    variants["rgb565_be_vflip"]   = np.flipud(b)

# Single-byte grayscale at a square root size (in case the camera sent 8-bit)
n = len(payload)
side = int(n ** 0.5)
if side >= 64:
    arr = np.frombuffer(payload[:side*side], dtype=np.uint8).reshape(side, side)
    variants[f"grayscale_{side}x{side}"] = np.stack([arr]*3, axis=-1)

# Treat as raw RGB888 at 104x104 (32768 / 3 ~= 10922, sqrt=104)
n3 = 104 * 104 * 3
if len(payload) >= n3:
    arr = np.frombuffer(payload[:n3], dtype=np.uint8).reshape(104, 104, 3)
    variants["rgb888_104x104"] = arr

# Treat as raw RGB888 at 128x85 (32768 / 3 / 128 ~= 85)
if len(payload) >= 128*85*3:
    arr = np.frombuffer(payload[:128*85*3], dtype=np.uint8).reshape(85, 128, 3)
    variants["rgb888_128x85"] = arr

print(f"\nWriting {len(variants)} PNGs to {out_dir}:")
for name, img in sorted(variants.items()):
    if isinstance(img, Image.Image):
        img.save(out_dir / f"{name}.png"); arr = np.array(img)
    else:
        arr = np.asarray(img)
        Image.fromarray(arr.astype(np.uint8), mode="RGB").save(out_dir / f"{name}.png")
    rv = float(arr.astype(np.float32).var(axis=(1,2)).mean())
    cv = float(arr.astype(np.float32).var(axis=(0,2)).mean())
    print(f"  {name:30s} row_var={rv:8.1f}  col_var={cv:8.1f}")
