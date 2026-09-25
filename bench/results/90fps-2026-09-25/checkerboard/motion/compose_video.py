#!/usr/bin/env python3
"""Add independent titles to CPU reconstructions and assemble 90 unique fps."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import subprocess

ROOT = Path(__file__).resolve().parent
RAW = ROOT / "frames"
OUT = ROOT / "composed"
OUT.mkdir(exist_ok=True)
regular = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 15)
bold = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 20)
cap = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 14)
labels = ["Full NXDF decode", "Checker + old half (11 ms)", "Startup / >50 ms gap: nearest fallback", "Received phase only (mode 2: 4 px cells)"]
for i in range(90):
    body = Image.open(RAW / f"frame-{i}.ppm").convert("RGB")
    image = Image.new("RGB", (body.width, body.height + 72), (10, 17, 27))
    d = ImageDraw.Draw(image)
    d.text((18, 5), "SYNTHETIC STRAIGHT EDGE + TEXT · 90 Hz", font=bold, fill=(241, 247, 255))
    d.text((18, 31), "CPU reconstruction from Vulkan-packed NXDF · native center included · mode-2 peripheral lattice = 4×4 output pixels",
           font=regular, fill=(131, 197, 226))
    d.text((image.width - 118, 8), f"{i/90:0.3f} s", font=bold, fill=(97, 230, 218))
    for col, label in enumerate(labels):
        x = col * 448 + 12
        d.text((x, 53), label, font=cap, fill=(255, 203, 129) if col == 3 else (202, 218, 237))
    image.paste(body, (0, 72))
    image.save(OUT / f"frame-{i:03d}.png", optimize=True)

image = Image.open(OUT / "frame-045.png")
image.save(ROOT / "motion-preview.png")
subprocess.run([
    "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-framerate", "90",
    "-i", str(OUT / "frame-%03d.png"), "-frames:v", "90", "-c:v", "libx264",
    "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p", "-r", "90",
    "-movflags", "+faststart", str(ROOT / "synthetic-motion-90fps.mp4")
], check=True)
print(f"wrote 90 unique 90 Hz frames to {ROOT / 'synthetic-motion-90fps.mp4'}")
