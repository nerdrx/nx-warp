#!/usr/bin/env python3
"""Offline summary for the paired 60-second round-colour blur captures."""
import json, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).parent
off, on = (ROOT / "round-colour-off-client.log", ROOT / "round-colour-on-client.log")
if len(sys.argv) == 3:
    off, on = map(Path, sys.argv[1:])
rows = [json.loads(x) for x in subprocess.check_output(
    [sys.executable, str(ROOT / "analyze_live.py"), str(off), str(on)], text=True
).splitlines()]
def delta(path):
    a, b = rows[0][path]["means"], rows[1][path]["means"]
    return {k: {"off": a[k], "on": b[k], "delta": b[k] - a[k],
                "percent": (100 * (b[k] - a[k]) / a[k]) if a[k] else None}
            for k in sorted(set(a) & set(b))}
summary = {"inputs": [str(off), str(on)], "analyzer": rows,
           "deltas": {"client": delta("client"), "decoder": delta("decoder")},
           "scope": {"pair": "one 60-second off/on capture",
                     "claims": "descriptive paired result, not a statistical proof",
                     "latency": "own GPU/source-offset measurements; not photon-to-photon latency"}}
print(json.dumps(summary, indent=2))
