#!/usr/bin/env python3
"""Compare a final Pico sample against its desktop reference; private files stay private."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

p = argparse.ArgumentParser()
p.add_argument("source", type=Path)
p.add_argument("desktop_rgba", type=Path)
p.add_argument("pico_rgba", type=Path)
args = p.parse_args()
out = Path(__file__).resolve().parent
gpu = np.fromfile(args.pico_rgba, np.uint8).reshape(2176, 4352, 4)
cpu = np.fromfile(args.desktop_rgba, np.uint8).reshape(2176, 4352, 4)
delta = np.abs(gpu.astype(np.int16) - cpu.astype(np.int16))
if delta.max() > 2:
    raise RuntimeError(f"Pico reference mismatch: max={delta.max()}, MAE={delta.mean()}")
box = (560, 80, 920, 340)
canvas = Image.new("RGB", (2160, 568), "#0c0818")
draw = ImageDraw.Draw(canvas)
font = ImageFont.truetype("DejaVuSans.ttf", 22)
imgs = [Image.open(args.source).convert("RGB"), Image.fromarray(cpu[:, :, :3]),
        Image.fromarray(gpu[:, :, :3])]
labels = ["Shifted RGB source", "Desktop ASTC decode", "Actual Pico GPU readback"]
for i, (title, img) in enumerate(zip(labels, imgs)):
    draw.text((i * 720 + 10, 10), title, font=font, fill="#eee6ff")
    canvas.paste(img.crop(box).resize((720, 520), Image.Resampling.NEAREST), (i * 720, 48))
canvas.save(out / "pico-readback-detail.png")
metadata = {"crop": box, "enlargement": "nearest2x",
            "raw_gpu_sha256": hashlib.sha256(args.pico_rgba.read_bytes()).hexdigest(),
            "raw_cpu_sha256": hashlib.sha256(args.desktop_rgba.read_bytes()).hexdigest(),
            "max_channel_error": int(delta.max()), "channel_mae": float(delta.mean()),
            "over_tolerance_channels": int((delta > 2).sum()),
            "note": "Final frame2 readback of offscreen compute sample, not a headset screenshot."}
(out / "pico-readback.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(metadata)
