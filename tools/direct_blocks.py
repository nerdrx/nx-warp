#!/usr/bin/env python3
"""Small, deterministic CPU reference for NX Warp direct display blocks."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


TILE = 32
RADII = (0.42, 0.72)


def fixture(w: int, h: int) -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    xx, yy = x / w, y / h
    r = 0.5 + 0.5 * np.sin(xx * 17 + yy * 4)
    g = 0.5 + 0.5 * np.sin(yy * 23 - xx * 5)
    b = 0.5 + 0.5 * np.sin((xx + yy) * 29)
    a = np.stack((r, g, b), -1)
    a[(x - .25*w) ** 2 + (y - .32*h) ** 2 < (.11*w) ** 2] = (1, .12, .08)
    a[(x - .73*w) ** 2 + (y - .60*h) ** 2 < (.16*w) ** 2] = (.08, .85, 1)
    a[(x + y * .8) % 97 < 7] = (1, .85, .08)
    im = Image.fromarray(np.uint8(np.clip(a * 255, 0, 255)), "RGB")
    d = ImageDraw.Draw(im)
    d.rectangle((4, 4, w - 5, h - 5), outline=(255, 255, 255), width=max(1, w // 256))
    d.text((w // 32, h // 2), "NX DIRECT 90", fill=(255, 255, 255), stroke_width=1)
    return np.asarray(im)


def rgb565(c: np.ndarray) -> int:
    r, g, b = (int(v) for v in c)
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)


def unpack565(v: int) -> np.ndarray:
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.array([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], dtype=np.int32)


def resize_box(tile: np.ndarray, n: int) -> np.ndarray:
    return np.asarray(Image.fromarray(tile).resize((n, n), Image.Resampling.BOX), dtype=np.uint8)


def block_encode(samples: np.ndarray) -> tuple[bytes, np.ndarray]:
    flat = samples.reshape(-1, 3).astype(np.int32)
    luma = flat @ np.array([299, 587, 114])
    c0 = flat[int(np.argmin(luma))]
    c1 = flat[int(np.argmax(luma))]
    p0, p1 = rgb565(c0), rgb565(c1)
    a, b = unpack565(p0), unpack565(p1)
    pal = np.stack([((3 - i) * a + i * b + 1) // 3 for i in range(4)])
    idx = np.argmin(((flat[:, None] - pal[None]) ** 2).sum(2), 1).astype(np.uint32)
    word0 = p0 | (p1 << 16)
    words = [word0, *[int(sum(int(idx[8 * row + col]) << (2 * (8 * row + col)) for row in range(8) for col in range(8) if 8 * row + col < 32)) for _ in [0]]]
    # Keep selectors as four little-endian uint32 words (16 selectors each).
    words = [word0] + [sum(int(idx[16 * k + i]) << (2 * i) for i in range(16)) for k in range(4)]
    return struct.pack("<5I", *words), idx


def encode(src: np.ndarray) -> tuple[bytes, bytes, np.ndarray]:
    h, w, _ = src.shape
    assert w % TILE == 0 and h % TILE == 0
    blocks: list[bytes] = []
    desc = [0] * ((w // TILE) * (h // TILE))
    decoded = np.zeros_like(src)
    eyes = w // 2
    for eye in range(2):
        view = src[:, eye * eyes:(eye + 1) * eyes]
        out = decoded[:, eye * eyes:(eye + 1) * eyes]
        cx, cy = eyes / 2, h / 2
        for ty in range(0, h, TILE):
            for tx in range(0, eyes, TILE):
                tile = view[ty:ty + TILE, tx:tx + TILE]
                radius = math.hypot((tx + 16 - cx) / (eyes / 2), (ty + 16 - cy) / (h / 2))
                scale = 1 if radius <= RADII[0] else 2 if radius <= RADII[1] else 4
                n = TILE // scale
                samples = resize_box(tile, n)
                first_word = len(blocks) * 5
                for by in range(0, n, 8):
                    for bx in range(0, n, 8):
                        raw, _ = block_encode(samples[by:by + 8, bx:bx + 8])
                        blocks.append(raw)
                        block = struct.unpack("<5I", raw)
                        p0, p1 = block[0] & 0xffff, block[0] >> 16
                        pal = np.stack([((3 - i) * unpack565(p0) + i * unpack565(p1) + 1) // 3 for i in range(4)])
                        ids = np.array([(block[1 + j // 16] >> (2 * (j % 16))) & 3 for j in range(64)])
                        recon = pal[ids].reshape(8, 8, 3).astype(np.uint8)
                        recon = np.repeat(np.repeat(recon, scale, axis=0), scale, axis=1)
                        out[ty + by * scale:ty + (by + 8) * scale,
                            tx + bx * scale:tx + (bx + 8) * scale] = recon
                di = (ty // TILE) * (w // TILE) + eye * (eyes // TILE) + tx // TILE
                desc[di] = first_word | (int(math.log2(scale)) << 30)
    descriptors = struct.pack("<%dI" % len(desc), *desc)
    return descriptors, b"".join(blocks), decoded


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--input", type=Path)
    args = ap.parse_args()
    if args.size <= 0 or args.size % TILE:
        raise SystemExit("--size must be divisible by 32")
    if args.input:
        one = np.asarray(Image.open(args.input).convert("RGB").resize((args.size, args.size), Image.Resampling.BOX))
    else:
        one = fixture(args.size, args.size)
    src = np.concatenate((one, one[:, ::-1]), axis=1)
    descriptors, blocks, decoded = encode(src)
    payload = descriptors + blocks
    args.out.mkdir(parents=True, exist_ok=True)
    Image.fromarray(src).save(args.out / "source.png")
    Image.fromarray(decoded).save(args.out / "decoded.png")
    np.concatenate((decoded, np.full((*decoded.shape[:2], 1), 255, dtype=np.uint8)), axis=2).tofile(args.out / "cpu.rgba")
    (args.out / "descriptors.bin").write_bytes(descriptors)
    (args.out / "blocks.bin").write_bytes(blocks)
    (args.out / "payload.bin").write_bytes(payload)
    mse = np.mean((src.astype(np.float64) - decoded.astype(np.float64)) ** 2)
    psnr = float("inf") if mse == 0 else 10 * math.log10(255 * 255 / mse)
    words = struct.unpack("<%dI" % (len(descriptors) // 4), descriptors)
    scale_counts = {str(s): sum((d >> 30) == s for d in words) for s in range(3)}
    tiles_per_eye = (args.size // TILE) ** 2
    eye_desc_bytes = len(descriptors) // 2
    eye_blocks_bytes = len(blocks) // 2
    foveated_eye_bytes = eye_desc_bytes + eye_blocks_bytes
    unfoveated_eye_bytes = eye_desc_bytes + tiles_per_eye * 16 * 20
    meta = {
        "format": "direct-blocks-v1", "dimensions": [int(src.shape[1]), int(src.shape[0])],
        "eye_dimensions": [args.size, args.size], "tile": 32, "foveation_radii": RADII,
        "descriptor_words": len(descriptors) // 4, "block_bytes": 20,
        "descriptor_bytes": len(descriptors), "blocks_bytes": len(blocks),
        "payload_bytes": len(payload), "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "stereo_bitrate_mbps": len(payload) * 8 * 90 / 1e6,
        "payload_bytes_per_eye": foveated_eye_bytes,
        "foveated_tile_fractions": {k: v / len(words) for k, v in scale_counts.items()},
        "unfoveated_payload_bitrate_mbps": unfoveated_eye_bytes * 2 * 8 * 90 / 1e6,
        "payload_bitrate_mbps": len(payload) * 8 * 90 / 1e6,
        "bitrate_scope": "payload only; excludes FEC, transport, and display headers",
        "refresh_hz": 90, "psnr_db_cpu_reference": psnr, "psnr_is_live_measurement": False,
        "descriptor_layout": "uint32 little-endian; offset in block-stream WORDS, low 30 bits; scale log2 top 2 bits",
        "block_layout": "20 bytes: RGB565 c0 low16/c1 high16, then 4 uint32 words with 64 2-bit selectors",
    }
    (args.out / "metadata.json").write_text(json.dumps(meta, indent=2) + "\n")
    assert len(blocks) % 20 == 0 and all(d & 0x3fffffff < len(blocks) // 4 for d in struct.unpack("<%dI" % (len(descriptors) // 4), descriptors))
    assert payload == descriptors + blocks and decoded.shape == src.shape
    assert np.array_equal(unpack565(0xF81F), np.array([255, 0, 255]))
    probe = np.arange(64, dtype=np.uint32) & 3
    probe_words = [sum(int(probe[16 * k + i]) << (2 * i) for i in range(16)) for k in range(4)]
    assert all(((probe_words[j // 16] >> (2 * (j % 16))) & 3) == probe[j] for j in range(64))


if __name__ == "__main__":
    main()
