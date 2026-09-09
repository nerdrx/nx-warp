#!/usr/bin/env python3
"""Offline summary for the paired mode-3 remap captures."""
import json, subprocess, sys
from pathlib import Path

root = Path(__file__).parent
off = root / "round-colour-remap-control-client.log"
on = root / "round-colour-remap-client.log"
if len(sys.argv) == 3:
    off, on = map(Path, sys.argv[1:])
rows = [json.loads(x) for x in subprocess.check_output(
    [sys.executable, str(root / "analyze_live.py"), str(on), str(off)], text=True
).splitlines()]
def delta(path):
    a, b = rows[1][path]["means"], rows[0][path]["means"]
    return {k: {"control": a[k], "remap": b[k], "delta": b[k] - a[k],
                "percent": 100 * (b[k] - a[k]) / a[k] if a[k] else None}
            for k in sorted(set(a) & set(b))}
print(json.dumps({"run_order": [str(on), str(off)], "analyzer": rows,
                  "deltas_remap_minus_control": {
                      "client": delta("client"), "decoder": delta("decoder")},
                  "scope": {"pair": "one 60-second mode-3 remap/control pair",
                            "claims": "descriptive paired result, not a statistical proof",
                            "latency": "own GPU/source-offset measurements; not photon-to-photon latency"}},
                 indent=2))
