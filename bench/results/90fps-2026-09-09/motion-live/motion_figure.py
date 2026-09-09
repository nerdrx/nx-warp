#!/usr/bin/env python3
"""Create a three-frame motion capture figure and exact RGB change counts."""
import json
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent
FRAMES = (("eye-start.png", "0.000 s"), ("eye-mid.png", "+9.015 s"),
          ("eye-later.png", "+19.030 s"))
THRESHOLD = 8


def rgb(name):
    return np.asarray(Image.open(ROOT / name).convert("RGB"), dtype=np.int16)


def changed(a, b):
    delta = np.max(np.abs(a - b), axis=2) > THRESHOLD
    whole = float(delta.mean() * 100)
    lo = (a.shape[0] - 512) // 2
    hi = lo + 512
    roi = float(delta[lo:hi, lo:hi].mean() * 100)
    return {"whole_image_percent": whole, "central_512_roi_percent": roi,
            "threshold_rgb_code": THRESHOLD, "pixels": int(delta.size),
            "central_512_bounds": [lo, hi, lo, hi]}


def main():
    images = [rgb(name) for name, _ in FRAMES]
    changes = {
        "threshold": "> 8/255 in any RGB channel",
        "captures": [{"file": name, "relative_time": time} for name, time in FRAMES],
        "comparisons": {
            "start_to_mid": changed(images[0], images[1]),
            "mid_to_later": changed(images[1], images[2]),
        },
        "note": "Captures are approximately 9–10 seconds apart; this is visual evidence, not a per-frame motion or photon-latency measurement.",
    }
    (ROOT / "changes.json").write_text(json.dumps(changes, indent=2) + "\n")

    fig, axes = plt.subplots(1, 3, figsize=(15, 5.4), constrained_layout=True)
    for axis, image, (_, time) in zip(axes, images, FRAMES):
        axis.imshow(image)
        axis.set_title(f"{time}  •  2160×2160 eye")
        axis.set_xlabel("pixels")
        axis.set_ylabel("pixels")
        axis.set_xticks([0, 512, 1080, 1648, 2159])
        axis.set_yticks([0, 512, 1080, 1648, 2159])
    fig.suptitle("Captured eye view across motion-live session")
    fig.text(0.5, 0.01, "No head-motion or photon-latency claim; captures are ~9–10 s apart.",
             ha="center", fontsize=10)
    fig.savefig(ROOT / "motion-captures.png", dpi=170)


if __name__ == "__main__":
    main()
