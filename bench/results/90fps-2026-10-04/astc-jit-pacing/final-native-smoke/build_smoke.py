#!/usr/bin/env python3
"""Rebuild final planar/RGB/planar smoke appendix from filtered captures."""
import csv, hashlib, json, re, shutil
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SCRATCH = Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-timeline-upload-20261004/live-check")
OUT = Path(__file__).resolve().parent
RUNS = [
    ("K", "final-planar-a-k", "planar", "warm since producer-pause run"),
    ("L", "final-rgb-b-l", "RGB", "fresh reconnect"),
    ("M", "final-planar-c-m", "planar", "fresh reconnect"),
]
RENDER = re.compile(r"render: (\d+) iterations in ([\d.]+) s \(([\d.]+)/s\), (\d+) submitted a layer, (\d+) new-source, (\d+) skipped by the repeat gate, (\d+) with nothing to show")
GPU = re.compile(r"this app's own GPU pass ([\d.]+) ms per iteration")
MISSES = re.compile(r"misses: (\d+) overrun (\d+) late (\d+) skipped refresh")
TS = re.compile(r"\[(2026-[^]]+)\]")

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

rows, captures = [], []
for label, name, mode, phase in RUNS:
    src = SCRATCH / name
    dst = OUT / label.lower()
    dst.mkdir(exist_ok=True)
    meta = json.loads((src / "metadata.json").read_text())
    files_to_copy = ["metadata.json", "client-extract.txt", "server-extract.txt", "server-stages.txt", "server-mode.txt", "server-mode-provenance.json"]
    for filename in files_to_copy:
        shutil.copyfile(src / filename, dst / filename)
    lines = (src / "client-extract.txt").read_text().splitlines()
    run_rows = []
    for line in lines:
        m = RENDER.search(line)
        if not m:
            continue
        stamp = TS.search(line).group(1)
        txt = "\n".join(x for x in lines if f"[{stamp}]" in x)
        gpu, misses = GPU.search(txt), MISSES.search(txt)
        iters, sec, ips, submitted, fresh, skip, nothing = m.groups()
        iters, sec, ips, fresh = int(iters), float(sec), float(ips), int(fresh)
        row = {
            "run": label, "mode": mode, "phase": phase, "device_time": stamp,
            "iterations": iters, "printed_duration_s": sec, "printed_iteration_rate_s": ips,
            "submitted_layers": int(submitted), "new_source": fresh,
            "estimated_new_source_rate_s": round(ips * fresh / iters, 4) if iters else "",
            "repeat_gate_skips": int(skip), "nothing_to_show": int(nothing),
            "own_gpu_ms": float(gpu.group(1)) if gpu else "",
            "missed_overrun_late_skipped": "/".join(misses.groups()) if misses else "",
            "window_scope": "steady_candidate" if iters >= 150 and 1.8 <= sec <= 2.2 else "partial_or_startup",
        }
        rows.append(row); run_rows.append(row)
    captures.append({
        "label": label, "scratch_capture": name, "mode": mode, "phase": phase,
        "metadata": meta,
        "files": {fn: {"bytes": (dst / fn).stat().st_size, "sha256": digest(dst / fn)}
                  for fn in files_to_copy},
        "steady_candidate_windows": sum(r["window_scope"] == "steady_candidate" for r in run_rows),
    })

with (OUT / "windows.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n"); w.writeheader(); w.writerows(rows)
(OUT / "capture-manifest.json").write_text(json.dumps(captures, indent=2, sort_keys=True) + "\n")

fig, ax = plt.subplots(figsize=(8, 4.4))
colors = {"K":"#1f77b4", "L":"#d62728", "M":"#2ca02c"}
for j, (label, _, mode, phase) in enumerate(RUNS):
    run = [r for r in rows if r["run"] == label and r["window_scope"] == "steady_candidate"]
    xs = [j + ((i % 5) - 2) * .06 for i in range(len(run))]
    ax.scatter(xs, [r["printed_iteration_rate_s"] for r in run], color=colors[label], marker="o", s=30, label="render iterations/s" if j == 0 else None)
    ax.scatter(xs, [float(r["estimated_new_source_rate_s"]) for r in run], color=colors[label], marker="x", s=30, label="estimated new-source/s" if j == 0 else None)
ax.axhline(90, color="#555", linestyle="--", linewidth=.8)
ax.set_xticks(range(3), ["K · planar\nwarm after pause", "L · RGB\nfresh reconnect", "M · planar\nfresh reconnect"])
ax.set_ylabel("Updates per second")
ax.set_ylim(75, 93)
ax.set_title("Final native-mode smoke runs — separate headset windows")
ax.grid(axis="y", alpha=.22)
ax.legend(frameon=False, loc="lower left")
fig.tight_layout()
fig.savefig(OUT / "native-mode-windows.png", dpi=160)
fig.savefig(OUT / "native-mode-windows.svg")
plt.close(fig)

# Keep the generated SVG clean for repository whitespace checks.
svg_path = OUT / 'native-mode-windows.svg'
svg_path.write_text("\n".join(line.rstrip() for line in svg_path.read_text().splitlines()) + "\n")
