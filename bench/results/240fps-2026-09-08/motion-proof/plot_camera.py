#!/usr/bin/env python3
"""Plot the retained three-second camera snippet against manifest yaw."""
import csv
import gzip
import io
import json
import tarfile
from pathlib import Path

import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
with tarfile.open(root / "input-evidence.tar", "r") as tar:
    members = {Path(m.name).name: m for m in tar.getmembers()}
    manifest = json.load(io.TextIOWrapper(tar.extractfile(members["camera-parallel.manifest.json"])))
    raw = tar.extractfile(members["camera-proof.csv.gz"]).read()
    rows = list(csv.DictReader(io.TextIOWrapper(gzip.GzipFile(fileobj=io.BytesIO(raw)), encoding="utf-8")))

yaw = [frame["camera_pose"]["yaw_deg"] for frame in manifest["source_frames"]]
timing = [float(row["total_ms"]) for row in rows]
frames = [int(row["frame"]) for row in rows]
if len(yaw) != len(timing) or len(yaw) != 720:
    raise SystemExit("camera plot requires the complete 720-frame, three-second snippet")

fig, ax = plt.subplots(figsize=(10, 4), constrained_layout=True)
ax.plot(frames, timing, color="#245a9a", linewidth=0.8, label="probe total_ms")
ax.set_xlabel("Camera fixture frame")
ax.set_ylabel("Scheduled-to-completion time (ms)")
ax.axhline(1000 / 240, color="black", linestyle="--", linewidth=0.8, label="240 FPS deadline")
ay = ax.twinx()
ay.plot(frames, yaw, color="#b34d27", linewidth=1.0, alpha=0.8, label="camera yaw")
ay.set_ylabel("Camera yaw (degrees)")
ax.set_title("Synthetic camera motion snippet: timing paired with manifest yaw (3 s)")
lines, labels = ax.get_legend_handles_labels()
lines2, labels2 = ay.get_legend_handles_labels()
ax.legend(lines + lines2, labels + labels2, loc="upper right", fontsize=8)
fig.savefig(root / "camera-timing-vs-yaw.png", dpi=170)
