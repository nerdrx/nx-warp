#!/usr/bin/env python3
"""Compare direct ASTC block payloads with a matching RGB source image."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

PREVIOUS_XUASTC_Q25_PSNR_DB = 33.253395034651845
PREVIOUS_XUASTC_Q25_LZ4_BYTES = 634704
DARK_SOURCE_SHA256 = "b2d08a36a284d5df467a5aaa119132e1358764fc595acdc12cd9db91723d94d3"
DEFAULT_DECODER = Path("/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/decode_astc")
DEFAULT_SOURCE = Path("/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/dark-native.png")
EDGE_CROP = (350, 80, 850, 480)  # pixels in native image coordinates; crop is not scaled


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def font(size: int):
    for candidate in ("/usr/share/fonts/TTF/DejaVuSans.ttf",
                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        if Path(candidate).exists():
            return ImageFont.truetype(candidate, size=size)
    return ImageFont.load_default()


def validate_astc(path: Path) -> tuple[int, int, int, int]:
    data = path.read_bytes()
    if len(data) < 16 or data[:4] != b"\x13\xAB\xA1\x5C":
        raise ValueError(f"{path}: invalid ASTC header/magic")
    bw, bh, bd = data[4:7]
    width = int.from_bytes(data[7:10], "little")
    height = int.from_bytes(data[10:13], "little")
    depth = int.from_bytes(data[13:16], "little")
    if not bw or not bh or bd != 1 or depth != 1 or not width or not height:
        raise ValueError(f"{path}: unsupported/invalid ASTC dimensions or block size")
    expected = 16 + ((width + bw - 1) // bw) * ((height + bh - 1) // bh) * 16
    if len(data) != expected:
        raise ValueError(f"{path}: invalid ASTC payload length {len(data)}, expected {expected}")
    return width, height, bw, bh


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("astc", nargs="+", type=Path, help="One or more standard .astc files")
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    ap.add_argument("--decoder", type=Path, default=DEFAULT_DECODER)
    ap.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent / "results")
    ap.add_argument("--lz4", action="append", default=[], metavar="ASTC_STEM=PATH",
                    help="Optional compressed raw-block LZ4 payload; repeat for multiple cases")
    args = ap.parse_args()
    if not args.decoder.is_file() or not args.source.is_file():
        ap.error("decoder or source image is missing")
    source_img = Image.open(args.source).convert("RGB")
    source = np.asarray(source_img, dtype=np.uint8)
    dark_baseline_applies = sha256(args.source) == DARK_SOURCE_SHA256
    args.output_dir.mkdir(parents=True, exist_ok=True)
    lz4_map: dict[str, Path] = {}
    for item in args.lz4:
        if "=" not in item:
            ap.error(f"--lz4 must be ASTC_STEM=PATH: {item}")
        key, value = item.split("=", 1)
        lz4_map[key] = Path(value)

    records = []
    for astc in args.astc:
        astc = astc.resolve()
        if not astc.is_file():
            raise FileNotFoundError(astc)
        width, height, block_w, block_h = validate_astc(astc)
        if source.shape[:2] != (height, width):
            raise ValueError(f"{astc}: source is {source.shape[1]}x{source.shape[0]}, ASTC is {width}x{height}")
        case = astc.stem
        decoded_path = args.output_dir / f"{case}.decoded.rgba"
        command = [str(args.decoder), str(astc), str(decoded_path)]
        proc = subprocess.run(command, capture_output=True, text=True)
        if proc.returncode != 0:
            decoded_path.unlink(missing_ok=True)
            raise RuntimeError(f"ASTC decoder failed ({proc.returncode}) for {astc}: {proc.stderr or proc.stdout}")
        expected_decoded = width * height * 4
        if not decoded_path.is_file() or decoded_path.stat().st_size != expected_decoded:
            raise RuntimeError(f"decoder output size mismatch for {astc}: expected {expected_decoded} RGBA bytes")
        decoded = np.fromfile(decoded_path, dtype=np.uint8).reshape(height, width, 4)
        delta = source.astype(np.int16) - decoded[:, :, :3].astype(np.int16)
        mae = float(np.abs(delta).mean())
        mse = float(np.square(delta.astype(np.float64)).mean())
        psnr = float("inf") if mse == 0 else 10.0 * math.log10((255.0 * 255.0) / mse)
        box = (min(EDGE_CROP[0], width), min(EDGE_CROP[1], height),
               min(EDGE_CROP[2], width), min(EDGE_CROP[3], height))
        if box[2] <= box[0] or box[3] <= box[1]:
            raise ValueError(f"image too small for edge crop: {width}x{height}")
        crop_size = (box[2] - box[0], box[3] - box[1])
        panel_h = crop_size[1] + 48
        panel = Image.new("RGB", (crop_size[0] * 2, panel_h), "#08060d")
        draw = ImageDraw.Draw(panel)
        draw.text((10, 8), "SOURCE · native 1:1", fill="#efeaff", font=font(17))
        draw.text((crop_size[0] + 10, 8), f"ASTC {block_w}×{block_h} · decoded 1:1", fill="#efeaff", font=font(17))
        panel.paste(source_img.crop(box), (0, 48))
        decoded_img = Image.fromarray(decoded[:, :, :3], mode="RGB")
        panel.paste(decoded_img.crop(box), (crop_size[0], 48))
        crop_path = args.output_dir / f"{case}.edge-1to1.png"
        panel.save(crop_path, optimize=True)

        compressed_path = lz4_map.get(case)
        if compressed_path is None:
            sibling = astc.with_suffix(astc.suffix + ".blocks.lz4")
            compressed_path = sibling if sibling.is_file() else None
        lz4_bytes = None
        if compressed_path is not None:
            if not compressed_path.is_file():
                raise FileNotFoundError(compressed_path)
            lz4_bytes = compressed_path.stat().st_size
        records.append({
            "astc_path": str(astc), "astc_sha256": sha256(astc),
            "width": width, "height": height, "block_width": block_w, "block_height": block_h,
            "source_path": str(args.source.resolve()), "source_sha256": sha256(args.source),
            "decoder_path": str(args.decoder.resolve()), "decoder_sha256": sha256(args.decoder),
            "decoder_command": command, "decoder_return_code": proc.returncode,
            "decoded_rgba_bytes": expected_decoded,
            "rgb_psnr_db": psnr, "rgb_mae": mae,
            "delta_psnr_vs_previous_xuastc_8x8_q25_db": psnr - PREVIOUS_XUASTC_Q25_PSNR_DB if dark_baseline_applies else None,
            "lz4_payload_path": str(compressed_path) if compressed_path else None,
            "lz4_payload_bytes": lz4_bytes,
            "delta_lz4_bytes_vs_previous_xuastc_8x8_q25": lz4_bytes - PREVIOUS_XUASTC_Q25_LZ4_BYTES if dark_baseline_applies and lz4_bytes is not None else None,
            "edge_crop_box_xyxy": list(box), "edge_crop_display_path": str(crop_path),
            "edge_crop_display_pixels": [crop_size[0] * 2, panel_h],
            "edge_crop_scale": 1,
        })
    payload = {
        "description": "Direct host ASTC block decode vs matching RGB8 source; this measures block quality only, not GPU decode correctness.",
        "baseline_reference": ({"format": "XUASTC 8x8 q25", "rgb_psnr_db": PREVIOUS_XUASTC_Q25_PSNR_DB,
                                 "lz4_raw_astc_blocks_bytes": PREVIOUS_XUASTC_Q25_LZ4_BYTES}
                                if dark_baseline_applies else None),
        "cases": records,
    }
    out = args.output_dir / "astc-quality.json"
    out.write_text(json.dumps(payload, indent=2, allow_nan=False) + "\n")
    summary = args.output_dir / "astc-quality.txt"
    summary.write_text("\n".join(
        f"{Path(r['astc_path']).name}: PSNR {r['rgb_psnr_db']:.4f} dB, MAE {r['rgb_mae']:.4f}, "
        f"LZ4 {r['lz4_payload_bytes'] if r['lz4_payload_bytes'] is not None else 'not supplied'} B, "
        f"crop {r['edge_crop_display_path']}"
        for r in records) + "\n")
    print(out)
    print(summary)
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
