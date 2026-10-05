#!/usr/bin/env python3
"""Draw recorded loopback poll wall times; no headset or recovery claim."""
import csv
from pathlib import Path
base = Path(__file__).resolve().parent
rows = list(csv.DictReader((base / "paired-poll.csv").open()))
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="610" viewBox="0 0 1100 610">', '<rect width="1100" height="610" fill="#111421"/>']
def text(x, y, value, size=15, color="#eeeaf8"):
    parts.append(f'<text x="{x}" y="{y}" font-family="sans-serif" font-size="{size}" fill="{color}">{value}</text>')
text(35, 38, "Removing the extra overdue poll wait — loopback component only", 25)
text(35, 68, "Two independent runs, 14 alternating pairs each • real typed sockets and extracted poll method", 15, "#b8b5ce")
colors = {"root": "#51d5b3", "luna": "#b89aff"}
for panel, upper, factor, title, unit in [(0, 1.15, 1e6, "Both timeout choices", "ms"), (1, 5.0, 1000, "Zero-wait choice, enlarged scale", "µs")]:
    left = 78 + panel * 535
    top, height, width = 137, 290, 425
    text(left, 112, title, 20)
    for tick in range(6):
        val = upper * tick / 5
        y = top + height * (1 - val / upper)
        parts.append(f'<path d="M{left},{y}h{width}" stroke="#30374d"/>')
        text(left-52, y+5, f"{val:.2f}", 13, "#b8b5ce")
    text(left-52, top-10, unit, 14, "#b8b5ce")
    for tick in [0, 3, 6, 9, 13]:
        x = left + width * tick / 13
        text(x-4, top+height+26, str(tick), 13, "#b8b5ce")
    text(left+140, top+height+53, "Pair index", 15, "#b8b5ce")
    for r in rows:
        if panel == 1 and r["mode"] != "local_candidate_0ms":
            continue
        val = int(r["poll_wall_ns"]) / factor
        x = left + width * int(r["pair"]) / 13
        y = top + height * (1-val/upper)
        color = colors[r["run"]]
        if r["mode"] == "baseline_1ms":
            parts.append(f'<rect x="{x-3}" y="{y-3}" width="6" height="6" fill="{color}"/>')
        else:
            parts.append(f'<circle cx="{x}" cy="{y}" r="3" fill="{color}"/>')
text(78, 530, "Green: root • Violet: Luna • Squares: existing 1 ms overdue wait • Circles: overdue-only 0 ms", 15)
text(35, 568, "Wall time includes scheduler behavior and clock overhead. Source hooks are stubbed; no actual NACK or RF recovery.", 14, "#b8b5ce")
text(35, 594, "Future deadlines, 2.5 ms quiet gating, two request rounds and default-off polling remain unchanged.", 14, "#b8b5ce")
parts.append("</svg>")
(base / "poll-wait.svg").write_text("\n".join(parts)+"\n")
