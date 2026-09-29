#!/usr/bin/env python3
"""Extract and plot the short live hybrid run from the committed log excerpts."""
import json
import re
from pathlib import Path

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
server = (ROOT / "server-telemetry.log").read_text()
client = (ROOT / "pico-telemetry.log").read_text()

encode = [(int(n), float(ms)) for n, ms in re.findall(
    r"stream 0 encoded (\d+) frames in 2\.0 s: ([\d.]+) ms/frame", server)]
jpeg = [(float(ms), float(kb)) for ms, kb in re.findall(
    r"JPEG periphery 544x544 compress ([\d.]+) ms, staging fence wait [\d.]+ ms, ([\d.]+) bytes/frame", server)]
decode = [float(us) / 1000 for us in re.findall(
    r"periphery JPEG rx=\d+ queued_drop=\d+ missing=\d+ decoded=\d+ invalid=\d+ decode_us_mean=([\d.]+)", client)]
applied = [(int(a), int(n)) for a, n in re.findall(
    r"NX JPEG periphery: applied (\d+)/(\d+) eye frames", client)]
offset = [float(ms) for ms in re.findall(r"source display-time offset ([\d.]+) ms mean", client)]
network = [int(n) for n in re.findall(r"nxwarp\[0\] net: (\d+) frames closed in 2\.0 s", client)]

def mean(values):
    return sum(values) / len(values) if values else None

summary = {
    "scenario": "headless Vulkan hello_xr full-field synthetic motion, WiVRn NX, Pico, 2176x2176 per eye, 90 Hz requested",
    "fresh_frames_per_second_mean": mean([n / 2 for n, _ in encode]),
    "server_encode_ms_mean": mean([ms for _, ms in encode]),
    "jpeg_compress_ms_mean": mean([ms for ms, _ in jpeg]),
    "jpeg_bytes_per_stereo_frame_mean": mean([b for _, b in jpeg]),
    "pico_jpeg_decode_ms_mean": mean(decode),
    "jpeg_applied_eye_frame_fraction": sum(a for a, _ in applied) / sum(n for _, n in applied),
    "source_display_offset_ms_mean": mean(offset),
    "network_completed_frames_per_second_mean": mean([n / 2 for n in network]),
    "windows": {"encode": len(encode), "jpeg_compress": len(jpeg), "decode": len(decode), "applied": len(applied)},
    "warning": "Source display-time offset is not physical motion-to-photon latency. Synthetic scene and off-head Pico are not a user quality test.",
}
(ROOT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

fig, ax = plt.subplots(3, 1, figsize=(9, 8), constrained_layout=True)
fig.patch.set_facecolor("#101321")
for a in ax:
    a.set_facecolor("#1a2030")
    a.tick_params(colors="#d8dcef")
    a.grid(color="#4c566e", alpha=.35)
    for spine in a.spines.values():
        spine.set_color("#4c566e")
ax[0].plot([n / 2 for n, _ in encode], color="#a882ff", marker="o", markersize=2)
ax[0].axhline(90, color="#79d9cc", linestyle="--", label="90 Hz target")
ax[0].set_ylabel("Fresh frames/s", color="white")
ax[0].legend(facecolor="#1a2030", labelcolor="white")
ax[1].plot([ms for _, ms in encode], color="#ffb56b", label="whole encode")
ax[1].plot([ms for ms, _ in jpeg], color="#a882ff", label="JPEG compression, sampled windows")
ax[1].axhline(1000/90, color="#79d9cc", linestyle="--", label="11.1 ms frame budget")
ax[1].set_ylabel("Server CPU ms/frame", color="white")
ax[1].legend(facecolor="#1a2030", labelcolor="white")
ax[2].plot(decode, color="#79d9cc", label="Pico JPEG decode")
ax[2].set_ylabel("Pico JPEG decode ms", color="white")
ax[2].set_xlabel("Consecutive reporting windows (independent series)", color="white")
right = ax[2].twinx()
right.plot([100*a/n for a, n in applied], color="#ffb56b", label="JPEG applied")
right.set_ylabel("JPEG applied, % eye frames", color="white")
right.tick_params(colors="#d8dcef")
ax[2].legend(facecolor="#1a2030", labelcolor="white", loc="upper left")
right.legend(facecolor="#1a2030", labelcolor="white", loc="upper right")
fig.suptitle("NXVC centre + Q20 JPEG periphery · live Pico short run", color="white", fontsize=15)
fig.savefig(ROOT / "live-telemetry.png", dpi=150)
