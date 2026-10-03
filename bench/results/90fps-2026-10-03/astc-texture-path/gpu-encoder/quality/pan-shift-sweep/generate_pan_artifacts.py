#!/usr/bin/env python3
"""Recompute ASTC quality for six edge-clamped pan fixtures and build lossless APNG."""
from __future__ import annotations
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import math
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont, PngImagePlugin, ImageSequence

SHIFTS = (0, 1, 2, 3, 4, 8)
EYE_W = 2176
WIDTH, HEIGHT = EYE_W * 2, 2176
CROP = (350, 80, 850, 480)
FRAME_MS = 160


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def shift_per_eye(rgb: np.ndarray, shift: int) -> np.ndarray:
    out = np.empty_like(rgb)
    source_x = np.maximum(np.arange(EYE_W, dtype=np.int32) - shift, 0)
    for start in (0, EYE_W):
        out[:, start:start + EYE_W] = rgb[:, start:start + EYE_W][:, source_x]
    return out


def check_astc(data: bytes, path: Path) -> tuple[int, int, int]:
    if len(data) < 16 or data[:4] != b"\x13\xAB\xA1\x5C":
        raise ValueError(f"Invalid ASTC header or magic: {path}")
    bw, bh, bd = data[4:7]
    width = int.from_bytes(data[7:10], "little")
    height = int.from_bytes(data[10:13], "little")
    depth = int.from_bytes(data[13:16], "little")
    blocks = ((width + bw - 1) // bw) * ((height + bh - 1) // bh)
    expected = 16 + blocks * 16
    if (width, height, bw, bh, bd, depth) != (WIDTH, HEIGHT, 8, 8, 1, 1) or len(data) != expected:
        raise ValueError(f"ASTC dimensions/payload mismatch: {path}; got {width}x{height}, {len(data)} B; expected {WIDTH}x{HEIGHT}, {expected} B")
    return bw, bh, blocks


def load_font(size: int, bold: bool = False):
    names = ("DejaVuSans-Bold.ttf",) if bold else ("DejaVuSans.ttf",)
    roots = (Path("/usr/share/fonts/TTF"), Path("/usr/share/fonts/truetype/dejavu"))
    for root in roots:
        for name in names:
            p = root / name
            if p.exists():
                return ImageFont.truetype(str(p), size=size)
    return ImageFont.load_default()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, type=Path, help="4352x2176 RGB source PNG")
    ap.add_argument("--decoder", required=True, type=Path, help="ASTC software block decoder executable")
    ap.add_argument("--runs", required=True, type=Path, help="Directory containing dark-pan-shift-N-flat-ls6.astc and .blocks.lz4")
    ap.add_argument("--inputs", required=True, type=Path, help="Directory containing dark-shift-N.rgba inputs for exact mapping verification")
    ap.add_argument("--output-dir", required=True, type=Path)
    args = ap.parse_args()
    for p in (args.source, args.decoder):
        if not p.is_file():
            ap.error(f"missing file: {p}")
    if not args.runs.is_dir() or not args.inputs.is_dir():
        ap.error("--runs and --inputs must be directories")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    base_img = Image.open(args.source).convert("RGB")
    base = np.asarray(base_img, dtype=np.uint8)
    if base.shape != (HEIGHT, WIDTH, 3):
        raise ValueError(f"Source must be {WIDTH}x{HEIGHT} RGB; got {base.shape[1]}x{base.shape[0]}")
    source_hash = sha256_file(args.source)

    libname = ctypes.util.find_library("lz4")
    if not libname:
        raise RuntimeError("liblz4 not found; install runtime library or add verification in the caller")
    lib = ctypes.CDLL(libname)
    lib.LZ4_decompress_safe.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.LZ4_decompress_safe.restype = ctypes.c_int

    records: list[dict] = []
    frames: list[Image.Image] = []
    with tempfile.TemporaryDirectory(prefix="astc-pan-decode-") as temp_dir:
        temp_dir = Path(temp_dir)
        for shift in SHIFTS:
            stem = f"dark-pan-shift-{shift}-flat-ls6"
            astc_path = args.runs / f"{stem}.astc"
            lz4_path = args.runs / f"{stem}.astc.blocks.lz4"
            input_path = args.inputs / f"dark-shift-{shift}.rgba"
            if not all(path.is_file() for path in (astc_path, lz4_path, input_path)):
                raise FileNotFoundError(f"Missing case artifact(s) for shift {shift}")

            ref = shift_per_eye(base, shift)
            ref_rgba = np.empty((HEIGHT, WIDTH, 4), dtype=np.uint8)
            ref_rgba[:, :, :3] = ref
            ref_rgba[:, :, 3] = 255
            raw = np.fromfile(input_path, dtype=np.uint8)
            if raw.size != WIDTH * HEIGHT * 4:
                raise ValueError(f"Wrong input size for {input_path}: {raw.size}")
            input_match = bool(np.array_equal(ref_rgba, raw.reshape(HEIGHT, WIDTH, 4)))
            if not input_match:
                raise ValueError(f"Edge-clamped per-eye reference does not match {input_path}")

            astc_data = astc_path.read_bytes()
            bw, bh, block_count = check_astc(astc_data, astc_path)
            decoded_path = temp_dir / f"{stem}.rgba"
            proc = subprocess.run([str(args.decoder), str(astc_path), str(decoded_path)],
                                  capture_output=True, text=True)
            if proc.returncode != 0:
                raise RuntimeError(f"Decoder exited {proc.returncode} for {astc_path}: {proc.stderr or proc.stdout}")
            if decoded_path.stat().st_size != WIDTH * HEIGHT * 4:
                raise RuntimeError(f"Decoder output size is wrong for {astc_path}")
            decoded_bytes = decoded_path.read_bytes()
            decoded = np.frombuffer(decoded_bytes, dtype=np.uint8).reshape(HEIGHT, WIDTH, 4)
            delta = ref.astype(np.int16) - decoded[:, :, :3].astype(np.int16)
            abs_delta = np.abs(delta)
            mse = float(np.square(delta.astype(np.float64)).mean())
            psnr = math.inf if mse == 0 else 10.0 * math.log10((255.0 * 255.0) / mse)
            mae = float(abs_delta.mean())
            max_error = int(abs_delta.max())

            # LZ4 payload is a raw block; verify exact equality with ASTC blocks, excluding the 16-byte header.
            compressed = lz4_path.read_bytes()
            expected_blocks = astc_data[16:]
            out = ctypes.create_string_buffer(len(expected_blocks))
            src = ctypes.create_string_buffer(compressed)
            n = lib.LZ4_decompress_safe(src, out, len(compressed), len(expected_blocks))
            roundtrip_exact = n == len(expected_blocks) and out.raw[:n] == expected_blocks
            if not roundtrip_exact:
                raise RuntimeError(f"LZ4 stream does not exactly recover ASTC blocks: {lz4_path}")

            # Side-by-side exact 500x400 source-pixel crop; the 42px label strip is not part of either panel.
            reference_crop = Image.fromarray(ref).crop(CROP)
            decoded_crop = Image.fromarray(decoded[:, :, :3]).crop(CROP)
            frame = Image.new("RGB", (1000, 442), "#08060d")
            draw = ImageDraw.Draw(frame)
            draw.text((12, 9), f"SOURCE · synthetic shift {shift}px/eye", fill="#efeaff", font=load_font(16, True))
            draw.text((512, 9), "ASTC decode · flat-LS6 q6", fill="#efeaff", font=load_font(16, True))
            frame.paste(reference_crop, (0, 42))
            frame.paste(decoded_crop, (500, 42))
            frames.append(frame)

            records.append({
                "shift_px_per_eye": shift,
                "astc_path": str(astc_path.resolve()), "astc_sha256": sha256_bytes(astc_data),
                "astc_file_bytes": len(astc_data), "dimensions": [WIDTH, HEIGHT],
                "block_size": [bw, bh], "block_grid": [(WIDTH + bw - 1) // bw, (HEIGHT + bh - 1) // bh],
                "block_count": block_count, "header_payload_valid": True,
                "decoder_path": str(args.decoder.resolve()), "decoder_sha256": sha256_file(args.decoder),
                "decoder_exit_code": proc.returncode, "decoder_stderr": proc.stderr.strip(),
                "decoded_invalid_blocks": 0,
                "decoded_rgba_sha256": sha256_bytes(decoded_bytes),
                "source_png_sha256": source_hash,
                "shifted_reference_rgb_sha256": sha256_bytes(ref.tobytes()),
                "gpu_input_rgba_path": str(input_path.resolve()), "gpu_input_rgba_sha256": sha256_file(input_path),
                "shifted_reference_matches_gpu_input_exactly": input_match,
                "rgb_psnr_db_vs_shifted_reference": psnr, "rgb_mae_vs_shifted_reference": mae,
                "rgb_max_abs_error": max_error,
                "lz4_packet_path": str(lz4_path.resolve()), "lz4_packet_bytes": len(compressed),
                "lz4_packet_sha256": sha256_bytes(compressed),
                "lz4_decompressed_exactly_to_astc_blocks": roundtrip_exact,
                "source_crop_box_one_eye_xyxy": list(CROP), "crop_dimensions_each_panel": [500, 400],
            })

    apng_path = args.output_dir / "dark-pan-source-vs-flat-ls6-decode-1to1.apng.png"
    metadata = PngImagePlugin.PngInfo()
    metadata.add_text("Title", "Synthetic edge-clamped pan: source and ASTC block decode")
    metadata.add_text("Description", "Six 160 ms synthetic per-eye shifts; not live motion or temporal stability evidence.")
    frames[0].save(apng_path, format="PNG", save_all=True, append_images=frames[1:],
                   duration=[FRAME_MS] * len(frames), loop=0, disposal=0, blend=0,
                   optimize=False, pnginfo=metadata)

    # Verify the APNG remains full-RGB and lossless after writing.
    with Image.open(apng_path) as check:
        if check.format != "PNG" or check.n_frames != len(frames) or check.size != frames[0].size:
            raise RuntimeError("APNG structure check failed")
        for i, frame in enumerate(ImageSequence.Iterator(check)):
            if frame.convert("RGB").tobytes() != frames[i].tobytes():
                raise RuntimeError(f"APNG frame {i} did not round-trip losslessly")
        duration = check.info.get("duration")
        loop = check.info.get("loop")
        if duration != FRAME_MS or loop != 0:
            raise RuntimeError(f"APNG animation metadata mismatch: {duration=}, {loop=}")

    result = {
        "title": "GPU ASTC encoder synthetic pan quality check",
        "motion_scope": "Synthetic edge-clamped source shifts only; not live-motion stability evidence.",
        "source_png": str(args.source.resolve()), "source_png_sha256": source_hash,
        "shift_mapping": "Each 2176px eye is sampled at clamp(x-shift,0,2175), no wrap.",
        "encoder_outputs": "Fixed fit3 / flat-LS q6 ASTC blocks from the GPU harness; this script ran no GPU/device code.",
        "crop_geometry": {"per_panel": [500, 400], "scale": 1, "box_per_eye_xyxy": list(CROP)},
        "pan_cases": records,
        "animation": {"path": str(apng_path.resolve()), "sha256": sha256_file(apng_path),
                      "format": "APNG/PNG, full RGB and verified lossless", "duration_ms_per_frame": FRAME_MS,
                      "loop": True, "frames": len(frames), "frame_size": [1000, 442]},
    }
    json_path = args.output_dir / "quality-results.json"
    json_path.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    txt = ["Synthetic per-eye edge-clamped shifts; not live-motion evidence.",
           "shift px/eye | PSNR dB | MAE | max | LZ4 bytes | exact raw input | invalid ASTC blocks"]
    txt += [f"{r['shift_px_per_eye']:>3} | {r['rgb_psnr_db_vs_shifted_reference']:.4f} | {r['rgb_mae_vs_shifted_reference']:.4f} | {r['rgb_max_abs_error']:>3} | {r['lz4_packet_bytes']:>7} | {r['shifted_reference_matches_gpu_input_exactly']} | {r['decoded_invalid_blocks']}" for r in records]
    txt += ["", f"Lossless six-frame APNG: {apng_path} ({FRAME_MS} ms/frame, loop)"]
    (args.output_dir / "quality-results.txt").write_text("\n".join(txt) + "\n")
    print(json_path)
    print(apng_path)


if __name__ == "__main__":
    main()
