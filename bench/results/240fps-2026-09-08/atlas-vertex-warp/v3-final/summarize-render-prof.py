#!/usr/bin/env python3
"""Summarize render-prof WiVRn reports as window aggregates.

The GPU values are already per-iteration window means; this deliberately does
not turn them into frame-level percentiles.
"""
import argparse, datetime as dt, json, re, statistics
from pathlib import Path

STAMP = re.compile(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
REPORT = re.compile(r"render: (\d+) iterations in ([0-9.]+) s .*?, (\d+) new-source,")
GPU = re.compile(r"render: this app's own GPU pass ([0-9.]+) ms per iteration")
CACHE = re.compile(r"defoveate (\d+)x(\d+) per eye x2 = ([0-9.]+) Mpx/frame .*?; (\d+) re-presented from the cache")

def parse(path, warmup=10.0):
    lines = path.read_text(errors="replace").splitlines()
    stamps = [STAMP.search(x).group(1) for x in lines if STAMP.search(x)]
    if not stamps:
        return {"file": str(path), "windows": []}
    parse_ts = lambda s: dt.datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f")
    start = parse_ts(min(stamps))
    windows = []
    pending = None
    incomplete = 0

    def finish():
        nonlocal pending, incomplete
        if pending is None:
            return
        if "gpu_ms_per_iteration" in pending and "cache_hits" in pending:
            windows.append(pending)
        else:
            incomplete += 1
        pending = None

    for line in lines:
        sm = STAMP.search(line)
        if not sm:
            continue
        timestamp = sm.group(1)
        elapsed = (parse_ts(timestamp) - start).total_seconds()
        m = REPORT.search(line)
        if m:
            # A new report starts a new ordered block.  This also makes
            # duplicate reports observable as incomplete rather than merged.
            finish()
            pending = {"timestamp": timestamp, "elapsed_s": elapsed,
                       "report_elapsed_s": elapsed,
                       "iterations": int(m.group(1)), "window_s": float(m.group(2)),
                       "fresh_new_source": int(m.group(3))}
            continue
        if pending is None:
            continue
        # Android logcat timestamps can advance between the three report
        # lines.  Keep the ordered block together for under one second.
        if elapsed - pending["report_elapsed_s"] >= 1.0:
            finish()
            continue
        m = GPU.search(line)
        if m:
            pending["gpu_ms_per_iteration"] = float(m.group(1))
            continue
        m = CACHE.search(line)
        if m:
            pending.update({"cache_hits": int(m.group(4)), "output_width": int(m.group(1)),
                            "output_height": int(m.group(2)), "output_mpix": float(m.group(3))})
            if "gpu_ms_per_iteration" in pending:
                finish()
    finish()
    windows = [w for w in windows if w["elapsed_s"] >= warmup]
    for w in windows:
        w["gpu_duty_ms_per_s"] = w["gpu_ms_per_iteration"] * w["iterations"] / w["window_s"]
        n = w["iterations"] - w["cache_hits"]
        w["estimated_noncache_gpu_ms"] = (w["gpu_ms_per_iteration"] * w["iterations"] / n
                                           if n > 0 else None)
    def med(k):
        vals = [w[k] for w in windows if w.get(k) is not None]
        return statistics.median(vals) if vals else None
    return {"file": str(path), "warmup_s": warmup, "windows": windows,
            "incomplete_blocks": incomplete,
            "window_count": len(windows),
            "median_window_gpu_ms": med("gpu_ms_per_iteration"),
            "median_gpu_duty_ms_per_s": med("gpu_duty_ms_per_s"),
            "median_estimated_noncache_gpu_ms": med("estimated_noncache_gpu_ms"),
            "median_fresh_new_source": med("fresh_new_source"),
            "output": ({"width": windows[0]["output_width"], "height": windows[0]["output_height"],
                        "mpix": windows[0]["output_mpix"]} if windows else None)}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix", nargs="*", help="render-prof prefix or measure.log")
    ap.add_argument("--warmup", type=float, default=10.0)
    ap.add_argument("-o", "--output", type=Path)
    args = ap.parse_args()
    paths = []
    for x in args.prefix or [str(Path(__file__).parent / "render-prof-*-measure.log")]:
        p = Path(x)
        paths += sorted(p.parent.glob(p.name)) if "*" in p.name else [p]
    result = {"warmup_s": args.warmup, "reports": [parse(p, args.warmup) for p in paths]}
    out = json.dumps(result, indent=2) + "\n"
    (args.output.write_text(out) if args.output else print(out, end=""))

if __name__ == "__main__":
    main()
