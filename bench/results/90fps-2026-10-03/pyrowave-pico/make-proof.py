#!/usr/bin/env python3
"""Build compact visual evidence from the local PyroWave capture files."""
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont


OUT = Path(__file__).resolve().parent
WIDTH, HEIGHT = 2688, 1344


def font(size):
    for candidate in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",):
        if Path(candidate).exists():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


def comparison():
    labels = ("Desktop reference", "Pico readback before flush", "Pico readback after flush")
    paths = ("/tmp/pyrowave-reference.png", "/tmp/pyrowave-pico.png", "/tmp/pyrowave-corrected.png")
    images = [Image.open(path).convert("RGB") for path in paths]
    if len({im.size for im in images}) != 1:
        raise ValueError("The three still-image inputs must have matching dimensions")
    panel_w, panel_h = 544, 272
    margin, gap, header = 24, 16, 58
    canvas = Image.new("RGB", (margin * 2 + panel_w * 3 + gap * 2, header + panel_h + margin), "#111820")
    draw = ImageDraw.Draw(canvas)
    for i, (label, image) in enumerate(zip(labels, images)):
        x = margin + i * (panel_w + gap)
        draw.text((x, 18), label, font=font(19), fill="#f2f5f7")
        canvas.paste(image.resize((panel_w, panel_h), Image.Resampling.LANCZOS), (x, header))
    canvas.save(OUT / "readback-comparison.png", optimize=True)


def motion_metrics_and_strip():
    raw_dir = Path("/tmp/pyrowave-motion-capture")
    y4m = Path("/tmp/pyrowave-motion-420-decoded.y4m")
    data = y4m.read_bytes()
    pos = data.index(b"\n") + 1
    sizes = (WIDTH * HEIGHT, WIDTH * HEIGHT // 4, WIDTH * HEIGHT // 4)
    metrics = []
    for frame in range(6):
        if not data.startswith(b"FRAME", pos):
            raise ValueError(f"Missing Y4M frame marker {frame}")
        pos = data.index(b"\n", pos) + 1
        row = []
        for plane, size in zip(("y", "cb", "cr"), sizes):
            if pos + size > len(data):
                raise ValueError(f"Truncated Y4M plane in frame {frame}")
            reference = np.frombuffer(data, dtype=np.uint8, count=size, offset=pos)
            pico = np.fromfile(raw_dir / f"frame{frame:03}.{plane}.raw", dtype=np.uint8)
            if pico.size != size:
                raise ValueError(f"Unexpected Pico capture size for frame {frame} {plane}")
            row.append(float(np.abs(reference.astype(np.int16) - pico.astype(np.int16)).mean()))
            pos += size
        metrics.append(row)

    with tempfile.TemporaryDirectory(prefix="pyrowave-proof-") as temp:
        pico_yuv = Path(temp) / "pico.yuv"
        with pico_yuv.open("wb") as output:
            for frame in range(6):
                for plane in ("y", "cb", "cr"):
                    output.write((raw_dir / f"frame{frame:03}.{plane}.raw").read_bytes())
        pattern = str(Path(temp) / "frame-%02d.png")
        subprocess.run(("ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
                        "-pix_fmt", "yuv420p", "-s", f"{WIDTH}x{HEIGHT}", "-i", str(pico_yuv),
                        "-frames:v", "6", "-vf", "scale=672:336:flags=lanczos", pattern), check=True)
        thumbs = [Image.open(Path(temp) / f"frame-{i + 1:02d}.png").convert("RGB") for i in range(6)]

    cell_w, cell_h, margin, gap, label_h = 672, 336, 20, 12, 32
    canvas = Image.new("RGB", (margin * 2 + cell_w * 3 + gap * 2,
                                margin * 2 + (cell_h + label_h) * 2 + gap), "#111820")
    draw = ImageDraw.Draw(canvas)
    for i, thumb in enumerate(thumbs):
        col, row = i % 3, i // 3
        x, y = margin + col * (cell_w + gap), margin + row * (cell_h + label_h + gap)
        draw.text((x, y), f"Frame {i + 1}", font=font(17), fill="#f2f5f7")
        canvas.paste(thumb, (x, y + label_h))
    canvas.save(OUT / "motion-contact-strip.png", optimize=True)
    for i, row in enumerate(metrics, 1):
        print(f"frame {i}: Y={row[0]:.6f} Cb={row[1]:.6f} Cr={row[2]:.6f} byte levels")


if __name__ == "__main__":
    comparison()
    motion_metrics_and_strip()
