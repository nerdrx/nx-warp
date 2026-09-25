#!/usr/bin/env python3
"""Create artificial moving-edge/text frames and pack them with the Vulkan fixture."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import argparse
import subprocess
import time

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "source"
ENCODED = ROOT / "encoded"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--fixture", required=True, type=Path,
                    help="path to the built direct_blocks_gpu_fixture executable")
FIXTURE = parser.parse_args().fixture.resolve()
W = H = 2160
COUNT = 90
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 92)
small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 36)
SOURCE.mkdir(parents=True, exist_ok=True)
ENCODED.mkdir(parents=True, exist_ok=True)
raw = SOURCE / "frame.rgba"
start = time.perf_counter()
for i in range(COUNT):
    im = Image.new("RGBA", (W, H), (9, 15, 24, 255))
    d = ImageDraw.Draw(im)
    for x in range(0, W, 64): d.line((x, 0, x, H), fill=(17, 27, 39, 255), width=1)
    for y in range(0, H, 64): d.line((0, y, W, y), fill=(17, 27, 39, 255), width=1)
    # The sharp edge moves 540 px in one second, crossing the native/periphery boundary.
    edge_x = 1020 + i * 6
    d.rectangle((edge_x - 4, 0, edge_x + 4, H), fill=(24, 230, 220, 255))
    d.line((0, 1080, W, 1080), fill=(40, 58, 74, 255), width=3)
    text_x = 850 + i * 6
    d.rounded_rectangle((text_x, 1000, text_x + 590, 1172), radius=18,
                        fill=(10, 19, 29, 245), outline=(70, 96, 119, 255), width=3)
    d.text((text_x + 24, 1010), "EDGE / TEXT MOTION", font=font,
           fill=(245, 249, 255, 255), stroke_width=1, stroke_fill=(0, 0, 0, 255))
    d.text((48, 48), f"SYNTHETIC SOURCE   {i:02d}/89   90 Hz", font=small,
           fill=(120, 205, 255, 255))
    if i in (0, 45, 89):
        im.convert("RGB").save(SOURCE / f"source-{i:03d}.png", optimize=True)
    raw.write_bytes(im.tobytes())
    del im, d
    target = ENCODED / f"frame-{i:03d}.nxdf"
    subprocess.run([str(FIXTURE), str(raw), str(target), "0", "1"],
                   check=True, stdout=subprocess.DEVNULL)
    if i % 15 == 0:
        print(f"packed {i + 1}/{COUNT}", flush=True)
raw.unlink(missing_ok=True)
print(f"packed {COUNT} synthetic source frames in {time.perf_counter() - start:.1f}s")
