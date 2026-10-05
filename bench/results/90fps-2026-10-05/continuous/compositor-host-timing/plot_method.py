#!/usr/bin/env python3
"""Source-derived capture method diagram. No measured durations are plotted."""
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

stages = [
    ("Retirement poll", "Zero-time fence poll; conditional", "#e9f5ed"),
    ("Acquire", "Output-image selection", "#eaf0fa"),
    ("Record", "Pool reset through cmd.end()", "#eaf0fa"),
    ("Queue lock", "Wait for submission mutex", "#faeaf0"),
    ("Submit", "Fence reset, submit, publication", "#eaf0fa"),
    ("Encoder present", "Calls into encoder, not display", "#eaf0fa"),
    ("Timeline wait", "Compute signal host wait", "#faeaf0"),
    ("Query wait", "getResults(eWait); conditional", "#faeaf0"),
    ("GC", "Deferred destruction; may block", "#faeaf0"),
]
fig, ax = plt.subplots(figsize=(11, 6.2))
fig.patch.set_facecolor("white")
ax.set_xlim(-.03, 3)
ax.set_ylim(-.35, 3.5)
ax.axis("off")
ax.text(0, 3.3, "Where the compositor's PC time goes", fontsize=20, weight="bold", color="#232136")
ax.text(0, 3.06, "Capture method: numbered call order, not measured duration", fontsize=12, color="#555365")
for i, (name, detail, color) in enumerate(stages):
    x, y = (i % 3) * 1.02, 2.1 - (i // 3) * .78
    box = FancyBboxPatch((x, y), .93, .62, boxstyle="round,pad=.01,rounding_size=.03", linewidth=1.2, edgecolor="#77728b", facecolor=color)
    ax.add_patch(box)
    ax.text(x + .06, y + .41, f"{i + 1}. {name}", fontsize=13, weight="bold", color="#302b43")
    ax.text(x + .06, y + .15, detail, fontsize=9, color="#4f4b5e")
ax.text(0, -.03, "Only WIVRN_DUMP_TIMINGS enables new clocks and CSV rows.", fontsize=11, color="#302b43")
ax.text(0, -.23, "Rows flush after measured intervals; capture can still perturb later frames. No FPS or photon-latency claim.", fontsize=10, color="#555365")
fig.tight_layout(pad=1.2)
out=Path(__file__).resolve().parent
fig.savefig(out / "capture-method.png", dpi=170)
fig.savefig(out / "capture-method.svg")
