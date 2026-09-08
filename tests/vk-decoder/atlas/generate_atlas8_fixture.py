#!/usr/bin/env python3
"""Write eight stereo YUV420 frames with a high-contrast moving luma block."""
import argparse
from pathlib import Path

WIDTH, HEIGHT, FRAMES = 256, 128, 8
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
args = parser.parse_args()
with args.output.open("wb") as output:
    for frame in range(FRAMES):
        luma = bytearray([32]) * (WIDTH * HEIGHT)
        left = 16 + frame * 8
        for row in range(24, 56):
            luma[row * WIDTH + left:row * WIDTH + left + 32] = bytes([220]) * 32
        output.write(luma)
        output.write(bytes([128]) * (WIDTH * HEIGHT // 2))
