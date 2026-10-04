#!/usr/bin/env python3
"""Offline centred 4:2:0 chroma reconstruction comparison for ASTC inputs."""
import csv
import math
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
FIXTURES = Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-half-rate-20261004/quality-improve/fixtures")
HARNESS = Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-half-rate-20261004/quality-improve/harness")
ENCODER = HARNESS / "build/astc-gpu"
DECODER = Path("/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/decode_astc")
SCENES = {
    "dark": "dark-left.png",
    "forest": "forest-left.png",
    "crowd": "crowd-500-reference.png",
}


def rgb_to_ycc(rgb):
    c = rgb.astype(np.float32) / 255.0
    r, g, b = np.moveaxis(c, -1, 0)
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    cb = (b - y) / 1.8556 + 0.5
    cr = (r - y) / 1.5748 + 0.5
    return y, cb, cr


def ycc_to_rgb(y, cb, cr):
    dcb, dcr = cb - 0.5, cr - 0.5
    r = y + 1.5748 * dcr
    g = y - 0.187324 * dcb - 0.468124 * dcr
    b = y + 1.8556 * dcb
    return np.rint(np.clip(np.stack((r, g, b), axis=-1), 0, 1) * 255).astype(np.uint8)


def reconstruct(y, cb420, cr420, mode):
    h, w = y.shape
    if mode == "nearest":
        cb = np.repeat(np.repeat(cb420, 2, axis=0), 2, axis=1)[:h, :w]
        cr = np.repeat(np.repeat(cr420, 2, axis=0), 2, axis=1)[:h, :w]
    else:
        # Chroma samples sit at centres of their 2x2 luma footprints.
        # Pixel-centre coordinates give chroma coordinates (pixel - 0.5) / 2.
        yy, xx = np.mgrid[0:h, 0:w]
        xf, yf = (xx - 0.5) / 2.0, (yy - 0.5) / 2.0
        x0, y0 = np.floor(xf).astype(int), np.floor(yf).astype(int)
        wx, wy = xf - x0, yf - y0

        def sample(plane):
            xa = np.clip(x0, 0, plane.shape[1] - 1)
            xb = np.clip(x0 + 1, 0, plane.shape[1] - 1)
            ya = np.clip(y0, 0, plane.shape[0] - 1)
            yb = np.clip(y0 + 1, 0, plane.shape[0] - 1)
            top = plane[ya, xa] * (1 - wx) + plane[ya, xb] * wx
            bot = plane[yb, xa] * (1 - wx) + plane[yb, xb] * wx
            return top * (1 - wy) + bot * wy

        cb, cr = sample(cb420), sample(cr420)
    return ycc_to_rgb(y, cb, cr)


def psnr(a, b):
    delta = a.astype(np.float32) - b.astype(np.float32)
    mse = float(np.mean(delta * delta))
    return float("inf") if mse == 0 else 10 * math.log10(255.0**2 / mse)


def font(size):
    for name in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"):
        if Path(name).is_file():
            return ImageFont.truetype(name, size)
    return ImageFont.load_default()


def main():
    out = ROOT / "results"
    out.mkdir(exist_ok=True)
    rows = []
    crowd_panels = []
    for scene, filename in SCENES.items():
        src_img = Image.open(FIXTURES / filename).convert("RGB")
        src = np.asarray(src_img, dtype=np.uint8)
        h, w = src.shape[:2]
        if w % 2 or h % 2:
            raise ValueError(f"{scene}: 4:2:0 dimensions must be even")
        y, cb, cr = rgb_to_ycc(src)
        cb420 = cb.reshape(h // 2, 2, w // 2, 2).mean((1, 3))
        cr420 = cr.reshape(h // 2, 2, w // 2, 2).mean((1, 3))
        versions = {"original_rgb": src}
        for mode in ("nearest", "bilinear"):
            versions[mode] = reconstruct(y, cb420, cr420, mode)

        for variant, image in versions.items():
            rgba = np.concatenate((image, np.full((h, w, 1), 255, dtype=np.uint8)), axis=2)
            for quality in (6, 3):
                prefix = out / f"{scene}-{variant}-q{quality}"
                raw = prefix.with_suffix(".rgba")
                raw.write_bytes(rgba.tobytes())
                astc = prefix.with_suffix(".astc")
                subprocess.run([str(ENCODER), str(raw), str(astc), str(w), str(h), "3", str(quality), "resident"],
                               cwd=HARNESS, check=True, stdout=subprocess.DEVNULL)
                decoded_path = out / f"{scene}-{variant}-q{quality}.decoded.rgba"
                subprocess.run([str(DECODER), str(astc), str(decoded_path)], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                decoded = np.fromfile(decoded_path, dtype=np.uint8).reshape(h, w, 4)[:, :, :3]
                blocks = astc.read_bytes()[16:]
                zst = prefix.with_suffix(".blocks.zst3")
                with zst.open("wb") as packed:
                    subprocess.run(["zstd", "-3", "--quiet", "--force", "-c"], input=blocks,
                                   stdout=packed, check=True)
                verify = subprocess.run(["zstd", "-d", "-q", "-c"], input=zst.read_bytes(),
                                        capture_output=True, check=True).stdout
                if verify != blocks:
                    raise RuntimeError(f"Zstd3 roundtrip mismatch: {zst}")
                rows.append({
                    "scene": scene, "quality": quality, "variant": variant, "width": w, "height": h,
                    "astc": astc.name, "zstd3_block_bytes": zst.stat().st_size,
                    "source_reconstruction_psnr_db": round(psnr(image, src), 5),
                    "decoded_vs_original_psnr_db": round(psnr(decoded, src), 5),
                    "decoded_vs_input_psnr_db": round(psnr(decoded, image), 5),
                })
                if scene == "crowd" and quality == 6:
                    box = (928, 250, 1248, 470)
                    crop = Image.fromarray(decoded).crop(box)
                    crowd_panels.append((variant, Image.fromarray(src).crop(box), crop))

    csv_path = out / "source-chroma.csv"
    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    width, height = 320, 220
    sheet = Image.new("RGB", (width * 5, height + 34), "#101015")
    draw = ImageDraw.Draw(sheet)
    by_name = {name: decoded for name, _, decoded in crowd_panels}
    box = (928, 250, 1248, 470)
    input_crops = {}
    for name in ("nearest", "bilinear"):
        raw = np.fromfile(out / f"crowd-{name}-q6.rgba", dtype=np.uint8).reshape(800, 2176, 4)
        input_crops[name] = Image.fromarray(raw[:, :, :3]).crop(box)
    panels = [
        ("Original source", Image.open(FIXTURES / SCENES["crowd"]).convert("RGB").crop(box)),
        ("4:2:0 nearest input", input_crops["nearest"]),
        ("4:2:0 bilinear input", input_crops["bilinear"]),
        ("Nearest · ASTC q6", by_name["nearest"]),
        ("Bilinear · ASTC q6", by_name["bilinear"]),
    ]
    for i, (label, panel) in enumerate(panels):
        sheet.paste(panel, (i * width, 34))
        draw.text((i * width + 8, 8), label + " · 1:1", fill="white", font=font(14))
    sheet.save(out / "crowd-chroma-edge-1to1.png", optimize=True)
    print(csv_path)
    print(out / "crowd-chroma-edge-1to1.png")


if __name__ == "__main__":
    main()
