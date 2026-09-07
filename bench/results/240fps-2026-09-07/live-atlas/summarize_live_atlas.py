#!/usr/bin/env python3
"""Summarize matched live ATLAS off/auto logs; exits 2 while a pair is absent."""
import argparse, hashlib, json, re, statistics, sys
from pathlib import Path
from datetime import datetime

NUM = r"(?:\d+(?:\.\d+)?|\.\d+)"

def values(text, pattern):
    return [float(x) for x in re.findall(pattern, text, re.I)]

def read_pair(scene, server):
    s = Path(scene).read_text(errors="replace")
    v = Path(server).read_text(errors="replace")
    render = values(s, rf"render: ({NUM}) iterations")
    submitted = [int(x) for x in re.findall(r"render: \d+ iterations.*?\b(\d+) submitted a layer", s, re.I)]
    display = values(s, rf"own GPU pass ({NUM}) ms")
    # These are the decoder's two-second windows; keep zeros as observed.
    window = rf"nxwarp\[\d+\]: \d+ frames in .*?"
    gpu = values(s, window + rf"gpu ({NUM}) ms")
    passa = values(s, window + rf"passA ({NUM})")
    passb = values(s, window + rf"passB ({NUM})")
    wall = values(s, rf"nxwarp\[\d+\]: \d+ frames in .*? wall ({NUM}) ms")
    new_source = [int(x) for x in re.findall(r"(\d+) new[- ]source", s, re.I)]
    new_source_rate = [float(n) / float(d) for d, n in re.findall(
        rf"render: \d+ iterations in ({NUM}) s.*?, (\d+) new-source", s, re.I)]
    # Server cadence is printed explicitly in encode windows.
    encoded = values(v, rf"encoded \d+ frames in ({NUM}) s")
    def stats(xs):
        if not xs: return {"count": 0, "p50": None, "p95": None, "zeros": 0, "outliers": []}
        ys = sorted(xs)
        p = lambda q: ys[min(len(ys)-1, max(0, int((len(ys)-1)*q + .5)))]
        med = statistics.median(ys)
        return {"count": len(xs), "p50": med, "p95": p(.95), "zeros": sum(x == 0 for x in xs),
                "outliers": [x for x in xs if x > (p(.95) * 1.5 if p(.95) else 0)]}
    digest = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
    reports = re.findall(r"^(\d\d-\d\d \d\d:\d\d:\d\d\.\d+).*render:.*?, (\d+) new-source", s, re.M)
    aligned = None
    if len(reports) > 1:
        times = [datetime.strptime("2026-" + t, "%Y-%m-%d %H:%M:%S.%f") for t, _ in reports]
        span = (times[-1] - times[0]).total_seconds()
        count = sum(int(n) for _, n in reports[1:])
        aligned = {"first_report": reports[0][0], "last_report": reports[-1][0],
                   "span_s": span, "count_excluding_first_report": count,
                   "reported_sources_per_wall_s": count / span if span > 0 else None,
                   "max_report_gap_s": max((b-a).total_seconds() for a,b in zip(times, times[1:])),
                   "note": "Observed reported counts; missing reports or session pauses are not reconstructed."}
    return {"aligned_report_span": aligned, "files": {"scene": str(scene), "server": str(server),
                       "scene_sha256": digest(scene), "server_sha256": digest(server)},
            "render_iterations": stats(render), "submitted_layers": stats(submitted),
            "display_gpu_ms": stats(display), "decoder_gpu_ms": stats(gpu),
            "decoder_wall_ms": stats(wall), "decoder_passA_ms": stats(passa), "decoder_passB_ms": stats(passb),
            "new_source": stats(new_source), "new_source_per_s": stats(new_source_rate),
            "server_reported_window_s": encoded,
            "startup_excluded": "only if caller removes the first explicit window"}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--off-scene", default="off-measure-filtered.log")
    ap.add_argument("--off-server", default="off-measure-filtered.log")
    ap.add_argument("--auto-scene", default="auto-measure-filtered.log")
    ap.add_argument("--auto-server", default="auto-measure-filtered.log")
    ap.add_argument("--out", default="live-atlas-summary.json")
    a = ap.parse_args()
    base = Path(__file__).parent
    paths = [base / x for x in (a.off_scene, a.off_server, a.auto_scene, a.auto_server)]
    if any(not p.is_file() for p in paths):
        print("waiting for matched off/auto logs", file=sys.stderr); return 2
    out = {"off": read_pair(paths[0], paths[1]), "auto": read_pair(paths[2], paths[3])}
    Path(a.out).write_text(json.dumps(out, indent=2) + "\n")
    return 0
if __name__ == "__main__": sys.exit(main())
