#!/usr/bin/env python3
"""Rebuild compact JIT pacing evidence from local headset extracts."""
from __future__ import annotations

import csv
import hashlib
import json
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SCRATCH = Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-timeline-upload-20261004/live-check")
OUT = Path(__file__).resolve().parent
RUNS = [
    ("A", "jit-cap500-a", "500 us cap; original APK"),
    ("B", "jit-cap2000-b", "2,000 us cap; original APK"),
    ("C", "jit-cap500-c", "500 us cap repeat; original APK"),
    ("D", "jit-original-probe-d", "45,000 us cap; original APK"),
    ("E", "jit-gradual-probe-e", "45,000 us cap; candidate APK"),
    ("F", "jit-gradual-warm-f", "45,000 us cap; candidate warm repeat"),
    ("G", "jit-gradual-pause-g", "45,000 us cap; controlled producer pause"),
    ("H", "jit-halfperiod-cold-h", "candidate default cap; cold-start run"),
    ("I", "jit-halfperiod-warm-i", "candidate default cap; warm repeat"),
    ("J", "jit-final86e0-pause-j", "clean final candidate; controlled producer pause"),
]

RENDER = re.compile(r"render: (\d+) iterations in ([\d.]+) s \(([\d.]+)/s\), (\d+) submitted a layer, (\d+) new-source, (\d+) skipped by the repeat gate, (\d+) with nothing to show")
GPU = re.compile(r"this app's own GPU pass ([\d.]+) ms per iteration")
OLDER = re.compile(r"selected older than available (\d+)")
SLEEP = re.compile(r"sleep cap ([\d.]+) ms")
MISSES = re.compile(r"misses: (\d+) overrun (\d+) late (\d+) skipped refresh")
TS = re.compile(r"\[(2026-[^]]+)\]")

def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

all_rows = []
capture_manifest = []
for label, name, description in RUNS:
    run = SCRATCH / name
    meta = json.loads((run / "metadata.json").read_text())
    lines = (run / "client-extract.txt").read_text().splitlines()
    windows = []
    for i, line in enumerate(lines):
        m = RENDER.search(line)
        if not m:
            continue
        stamp = TS.search(line).group(1)
        same_window = [x for x in lines if f"[{stamp}]" in x]
        txt = "\n".join(same_window)
        gpu = GPU.search(txt)
        older = OLDER.search(txt)
        sleep = SLEEP.search(txt)
        misses = MISSES.search(txt)
        iterations, duration, rate, submitted, fresh, repeat_gate, nothing = m.groups()
        iterations, duration, rate = int(iterations), float(duration), float(rate)
        fresh = int(fresh)
        row = {
            "run": label,
            "scratch_capture": name,
            "apk_sha256": meta.get("apk_sha256", ""),
            "device_time": stamp,
            "iterations": iterations,
            "printed_duration_s": duration,
            "printed_iteration_rate_s": rate,
            "submitted_layers": int(submitted),
            "new_source": fresh,
            "estimated_new_source_rate_s": round(rate * fresh / iterations, 4) if iterations else "",
            "repeat_gate_skips": int(repeat_gate),
            "nothing_to_show": int(nothing),
            "own_gpu_ms": float(gpu.group(1)) if gpu else "",
            "selected_older_than_available": int(older.group(1)) if older else "",
            "sleep_cap_ms": float(sleep.group(1)) if sleep else "",
            "missed_overrun_late_skipped": "/".join(misses.groups()) if misses else "",
            "window_scope": "steady_candidate" if iterations >= 150 and 1.8 <= duration <= 2.2 else "partial_or_startup",
        }
        windows.append(row)
        all_rows.append(row)
    capture_manifest.append({
        "label": label,
        "scratch_capture": name,
        "description": description,
        "metadata": meta,
        "client_extract_sha256": sha(run / "client-extract.txt"),
        "client_capture_sha256": sha(run / "client-capture.log"),
        "server_extract_sha256": sha(run / "server-extract.txt"),
        "server_stages_sha256": sha(run / "server-stages.txt"),
        "window_count": len(windows),
        "steady_candidate_windows": sum(w["window_scope"] == "steady_candidate" for w in windows),
    })

fields = list(all_rows[0])
with (OUT / "client-windows.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
    w.writeheader(); w.writerows(all_rows)
(OUT / "capture-manifest.json").write_text(json.dumps(capture_manifest, indent=2, sort_keys=True) + "\n")

gradual_packaging = json.loads(Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-timeline-upload-20261004/jit-gradual-packaging/manifest.json").read_text())
packaging = json.loads(Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-timeline-upload-20261004/jit-halfperiod-packaging/manifest.json").read_text())
final_packaging = json.loads(Path("/run/media/nerdrx/Lex/claude/nx-scratch/astc-timeline-upload-20261004/final86e0-packaging/manifest.json").read_text())
source_artifacts = {
    "capture_apk_sha256": {x["label"]: x["metadata"].get("apk_sha256", "") for x in capture_manifest},
    "candidate_e_g_packaging": {
        "apk_sha256": gradual_packaging["artifact_sha256"],
        "native_sha256": gradual_packaging["native_sha256"],
        "source_commit": gradual_packaging["source_commit"],
        "source_dirty": gradual_packaging["source_dirty"],
        "client_source_sha256": gradual_packaging["client_source_sha256"],
    },
    "candidate_h_i_packaging": {
        "apk_sha256": packaging["artifact_sha256"],
        "native_sha256": packaging["native_sha256"],
        "base_apk_sha256": packaging["base_sha256"],
        "certificate_sha256": packaging["certificate_sha256"],
        "source_commit": packaging["source_commit"],
        "source_dirty": packaging["source_dirty"],
        "client_source_sha256": packaging["client_source_sha256"],
        "other_apk_entry_contents_unchanged": packaging["all_other_entry_contents_unchanged"],
    },
    "final_clean_candidate_j_packaging": {
        "apk_sha256": final_packaging["artifact_sha256"],
        "native_sha256": final_packaging["native_sha256"],
        "base_apk_sha256": final_packaging["base_sha256"],
        "certificate_sha256": final_packaging["certificate_sha256"],
        "source_commit": final_packaging["source_commit"],
        "source_dirty": final_packaging["source_dirty"],
        "client_source_sha256": final_packaging["client_source_sha256"],
        "other_apk_entry_contents_unchanged": final_packaging["all_other_entry_contents_unchanged"],
    },
    "note": "E-G, H-I, and J APK digests are tied to included packaging manifests. A-D APK digests are recorded from per-run metadata; they are separate builds.",
}
(OUT / "source-artifacts.json").write_text(json.dumps(source_artifacts, indent=2, sort_keys=True) + "\n")

# Device-clock event order from G. Host stop/continue events remain UTC evidence;
# clocks are not aligned, so keep them in separate files/columns.
g = SCRATCH / "jit-gradual-pause-g"
events = []
for line in (g / "client-capture.log").read_text().splitlines():
    if "NXPause" in line or "Stream state" in line or "Session state changed" in line:
        stamp = TS.search(line)
        if stamp and stamp.group(1) >= "2026-10-04 08:19:07":
            events.append({"device_log_time": stamp.group(1), "event": line.split(" : ", 1)[-1]})
with (OUT / "device-events-g.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=["device_log_time", "event"], lineterminator="\n")
    w.writeheader(); w.writerows(events)
host_events = json.loads((g / "events.json").read_text())
(OUT / "host-events-g.json").write_text(json.dumps({"clock_note": "Host UTC timestamps are not aligned to headset log clock.", "events": host_events}, indent=2) + "\n")

j = SCRATCH / "jit-final86e0-pause-j"
events_j = []
for line in (j / "client-capture.log").read_text().splitlines():
    if "NXPause" in line or "Stream state" in line or "Session state changed" in line:
        stamp = TS.search(line)
        if stamp and stamp.group(1) >= "2026-10-04 08:37:29":
            events_j.append({"device_log_time": stamp.group(1), "event": line.split(" : ", 1)[-1]})
with (OUT / "device-events-j.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=["device_log_time", "event"], lineterminator="\n")
    w.writeheader(); w.writerows(events_j)
host_events_j = json.loads((j / "events.json").read_text())
(OUT / "host-events-j.json").write_text(json.dumps({"clock_note": "Host UTC timestamps are not aligned to headset log clock.", "events": host_events_j}, indent=2) + "\n")

def median(values):
    values = sorted(values)
    n = len(values)
    return values[n // 2] if n % 2 else (values[n // 2 - 1] + values[n // 2]) / 2

summary = []
for label, name, desc in RUNS:
    rows = [r for r in all_rows if r["run"] == label and r["window_scope"] == "steady_candidate"]
    if not rows:
        continue
    rates = [float(r["printed_iteration_rate_s"]) for r in rows]
    fresh = [float(r["estimated_new_source_rate_s"]) for r in rows]
    gpu = [float(r["own_gpu_ms"]) for r in rows if r["own_gpu_ms"] != ""]
    misses = sum(1 for r in rows if r["missed_overrun_late_skipped"] and any(int(x) for x in r["missed_overrun_late_skipped"].split("/")))
    summary.append({
        "run": label, "scratch_capture": name, "description": desc,
        "complete_windows": len(rows),
        "iteration_rate_median_s": round(median(rates), 2),
        "iteration_rate_min_s": min(rates), "iteration_rate_max_s": max(rates),
        "estimated_fresh_rate_median_s": round(median(fresh), 2),
        "own_gpu_min_ms": min(gpu) if gpu else "", "own_gpu_max_ms": max(gpu) if gpu else "",
        "windows_with_nonzero_deadline_counts": misses,
    })
with (OUT / "run-summary.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=list(summary[0]), lineterminator="\n")
    w.writeheader(); w.writerows(summary)

# Per-window points; no pooled or cross-session error bars.
palette = {"A":"#1f77b4", "B":"#ff7f0e", "C":"#2ca02c", "D":"#d62728", "E":"#9467bd", "F":"#8c564b", "G":"#e377c2", "H":"#17becf", "I":"#7f7f7f", "J":"#bcbd22"}
fig, (ax, gpu_ax) = plt.subplots(2, 1, figsize=(12, 7), sharex=True, gridspec_kw={"height_ratios": [2, 1]})
for j, (label, _, desc) in enumerate(RUNS):
    rows = [r for r in all_rows if r["run"] == label and r["window_scope"] == "steady_candidate"]
    if not rows:
        continue
    offsets = [((k % 5) - 2) * .055 for k in range(len(rows))]
    xs = [j + o for o in offsets]
    c = palette[label]
    ax.scatter(xs, [r["printed_iteration_rate_s"] for r in rows], s=24, color=c, marker="o", label="render iterations" if j == 0 else None)
    ax.scatter(xs, [float(r["estimated_new_source_rate_s"]) for r in rows], s=24, color=c, marker="x", label="estimated fresh-source updates" if j == 0 else None)
    gpu_rows = [(x, float(r["own_gpu_ms"])) for x, r in zip(xs, rows) if r["own_gpu_ms"] != ""]
    if gpu_rows:
        gpu_ax.scatter([x for x, _ in gpu_rows], [y for _, y in gpu_rows], color=c, marker="o", s=24)
ax.axhline(90, color="#555", linewidth=.8, linestyle="--")
ax.set_ylabel("Updates per second")
ax.set_ylim(0, 100)
ax.grid(axis="y", alpha=.22)
ax.legend(loc="lower left", frameon=False)
gpu_ax.set_ylabel("App GPU pass (ms)")
gpu_ax.set_xlabel("Capture (per-window values; not a controlled motion test)")
xticklabels = ["A\n0.5 ms", "B\n2 ms", "C\n0.5 repeat", "D\n45 ms old", "E\n45 ms cand.", "F\n45 ms warm", "G\npause", "H\ncold", "I\nwarm", "J\nclean pause"]
gpu_ax.set_xticks(range(len(RUNS)), xticklabels)
gpu_ax.tick_params(axis="x", labelsize=9)
gpu_ax.grid(axis="y", alpha=.22)
gpu_ax.set_ylim(0, 8)
fig.suptitle("Pico JIT pacing probes — independent headset windows")
fig.tight_layout()
fig.savefig(OUT / "pacing-windows.png", dpi=160)
fig.savefig(OUT / "pacing-windows.svg")
plt.close(fig)

# Keep the generated SVG clean for repository whitespace checks.
svg_path = OUT / 'pacing-windows.svg'
svg_path.write_text("\n".join(line.rstrip() for line in svg_path.read_text().splitlines()) + "\n")
