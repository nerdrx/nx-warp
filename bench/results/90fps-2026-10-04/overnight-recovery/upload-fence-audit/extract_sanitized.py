#!/usr/bin/env python3
"""Extract only ASTC prior-upload fence 180-frame means from client capture logs."""
import argparse
import csv
from pathlib import Path
import re

pattern = re.compile(r"prior-upload fence ([0-9.]+),.*\((same-queue async|sync);")
parser = argparse.ArgumentParser()
parser.add_argument("capture_root", type=Path, help="directory containing per-run client-capture.log files")
parser.add_argument("output", type=Path)
args = parser.parse_args()
values = {"same-queue async": [], "sync": []}
for path in sorted(args.capture_root.glob("*/client-capture.log")):
    for line in path.read_text(errors="ignore").splitlines():
        if "ASTC worker 180-frame mean us/frame" not in line:
            continue
        match = pattern.search(line)
        if match:
            values[match.group(2)].append(float(match.group(1)))
with args.output.open("w", newline="") as handle:
    writer = csv.writer(handle)
    writer.writerow(["mode", "window_index", "prior_upload_fence_180_frame_mean_us"])
    for mode, samples in values.items():
        for i, sample in enumerate(samples, 1):
            writer.writerow([mode, i, f"{sample:.1f}"])
print(f"async_windows={len(values['same-queue async'])} sync_windows={len(values['sync'])}")
