#!/usr/bin/env python3
"""Compare retained native planes and render their source comparison."""
from pathlib import Path
import argparse
import numpy as np
from PIL import Image, ImageOps
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser()
parser.add_argument("scratch", type=Path)
args = parser.parse_args()
source = np.fromfile(args.scratch / "private-dark-native-stereo-444.yuv", np.uint8)
planes = []
for i, name in enumerate(("y", "cb", "cr")):
    host = np.fromfile(args.scratch / f"host-copyfix.frame000.{name}.raw", np.uint8)
    pico = np.fromfile(args.scratch / f"pico-copyfix.frame000.{name}.raw", np.uint8)
    assert host.size == pico.size == 9469952
    error = np.abs(pico.astype(np.int16) - host.astype(np.int16))
    assert error.max() <= 1, f"{name}: hardware reference mismatch"
    reference = source[i * pico.size:(i + 1) * pico.size].astype(float)
    mse = np.mean((pico - reference) ** 2)
    print(f"{name}: MAE={error.mean():.6f}, max={error.max()}, PSNR={10*np.log10(255**2/mse):.3f}")
    planes.append(pico.reshape(2176, 4352)[:, :2176].astype(float))
y, cb, cr = planes
cb -= 128
cr -= 128
rgb = np.stack((y + 1.5748 * cr, y - .187324 * cb - .468124 * cr, y + 1.8556 * cb), axis=-1)
original = ImageOps.fit(Image.open(args.scratch / "private-dark-s0-source.png").convert("RGB"),
                        (2176, 2176), method=Image.Resampling.LANCZOS)
fig, axes = plt.subplots(1, 2, figsize=(9, 5), layout="constrained")
for ax, img, title in zip(axes, (original, np.uint8(np.clip(np.rint(rgb), 0, 255))),
                          ("Source (one eye)", "Pico native 4:4:4 decode")):
    ax.imshow(img)
    ax.set_title(title)
    ax.axis("off")
fig.suptitle("Full-image PyroWave · ~500 Mbit/s equivalent at 90 Hz\nSingle-image readback; not live playback")
fig.savefig(Path(__file__).parent / "feature-correct-native-readback.png", dpi=150)
