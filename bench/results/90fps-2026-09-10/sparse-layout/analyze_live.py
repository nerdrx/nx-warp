#!/usr/bin/env python3
"""Analyze four sparse-layout live captures in ABBA order."""
import json, re, sys
from pathlib import Path

ORDER = [("BaseA", "sparse-layout-base-a-client.log"),
         ("StaticA", "sparse-layout-candidate-a-client.log"),
         ("StaticB", "sparse-layout-candidate-b-client.log"),
         ("BaseB", "sparse-layout-base-b-client.log")]

def parse(path):
    client, decoder, c, d = [], [], {}, {}
    def num(p, s):
        m = re.search(p, s); return float(m.group(1)) if m else None
    def seconds(t):
        h, m, s = t.split(":"); return int(h)*3600 + int(m)*60 + float(s)
    for line in Path(path).read_text(errors="replace").splitlines():
        tm = re.search(r"(\d\d:\d\d:\d\d\.\d+)", line)
        t = tm.group(1) if tm else None
        if "render:" in line:
            m = re.search(r"render: (\d+) iterations in ([0-9.]+) s", line)
            if m:
                if c.get("time"): client.append(c)
                c = {"time": t, "elapsed": float(m.group(2))}
                f = re.search(r"(\d+) new-source", line)
                c["fresh"] = int(f.group(1))/c["elapsed"] if f else None
            elif c and "source display-time offset" in line:
                c["offset"] = num(r"offset ([0-9.-]+) ms", line)
        if "nxwarp[" in line:
            m = re.search(r"nxwarp\[\d+\]: (\d+) frames in ([0-9.]+) s:", line)
            if m:
                if d.get("time"): decoder.append(d)
                d = {"time": t, "passA": num(r"passA ([0-9.]+)", line),
                     "gpu": num(r"gpu ([0-9.]+) ms", line)}
    if c.get("time"): client.append(c)
    if d.get("time"): decoder.append(d)
    if not client or not decoder: raise SystemExit(f"no metrics in {path}")
    start = seconds(client[0]["time"])
    client = [r for r in client if (seconds(r["time"])-start) % 86400 >= 10 and r.get("fresh") is not None and r.get("offset") is not None]
    start = seconds(decoder[0]["time"])
    decoder = [r for r in decoder if (seconds(r["time"])-start) % 86400 >= 10 and r.get("passA") is not None and r.get("gpu") is not None]
    if not client or not decoder: raise SystemExit(f"missing 10s warm-window metrics in {path}")
    mean = lambda rows, key: sum(r[key] for r in rows)/len(rows)
    return {"passA_ms": mean(decoder, "passA"), "decode_gpu_ms": mean(decoder, "gpu"),
            "source_offset_ms": mean(client, "offset"), "fresh_per_s": mean(client, "fresh"),
            "n_client": len(client), "n_decoder": len(decoder)}

def main(argv):
    root = Path(__file__).parent.parent / "motion-live"
    paths = [Path(x) for x in argv] if argv else [root / name for _, name in ORDER]
    if len(paths) != 4: raise SystemExit("expected exactly four ABBA client logs")
    result = [{"label": label, "file": str(p), "means": parse(p)} for (label, _), p in zip(ORDER, paths)]
    Path(__file__).with_name("live-summary.json").write_text(json.dumps(result, indent=2)+"\n")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(2, 2, figsize=(9, 6), constrained_layout=True)
    for ax, (key, title) in zip(axes.flat, [("passA_ms", "Pass A (ms)"), ("decode_gpu_ms", "Decode GPU (ms)"), ("source_offset_ms", "Source offset (ms)"), ("fresh_per_s", "Fresh sources/s")]):
        vals = [r["means"][key] for r in result]
        if any(v is None for v in vals): raise SystemExit(f"missing {key}")
        ax.bar([r["label"] for r in result], vals); ax.set_title(title); ax.tick_params(axis="x", rotation=30)
    fig.savefig(Path(__file__).with_name("live-summary.png"), dpi=140)
    print(json.dumps(result, separators=(",", ":")))

if __name__ == "__main__": main(sys.argv[1:])
