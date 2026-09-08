#!/usr/bin/env python3
"""Summarize the stored D128 ablation logs (stdlib only)."""
import json, re, tarfile
from pathlib import Path

ROOT = Path(__file__).parent
LINE = re.compile(r"frame (\d+):.*?passB ([0-9.]+).*?gpu ([0-9.]+).*?total ([0-9.]+)")

def nr(values, q):
    if not values: return None
    a = sorted(values)
    return a[max(0, min(len(a) - 1, int((q * len(a) + 99) // 100) - 1))]

def main():
    out = {"fixture": "D128", "motion_frames": [25, 95], "runs": {}}
    archive = ROOT / "raw-logs.tar.gz"
    with tarfile.open(archive, "r:gz") as tar:
      sources = [(Path(m.name).name, tar.extractfile(m).read().decode(errors="replace"))
                 for m in tar.getmembers() if m.isfile() and m.name.endswith(".log")]
    for name, content in sorted(sources):
        rows = []
        for line in content.splitlines():
            m = LINE.search(line)
            if m:
                f = int(m.group(1))
                if 25 <= f <= 95:
                    rows.append({"frame": f, "passB_ms": float(m.group(2)),
                                 "gpu_ms": float(m.group(3)), "total_ms": float(m.group(4))})
        if not rows: continue
        def stats(key):
            v = [r[key] for r in rows]
            return {"count": len(v), "median_ms": nr(v, 50),
                    "p95_ms": nr(v, 95), "p99_ms": nr(v, 99)}
        name = Path(name).stem
        out["runs"][name] = {"frames": [rows[0]["frame"], rows[-1]["frame"]],
                              "stats": {k: stats(k) for k in ("passB_ms", "gpu_ms", "total_ms")}}
    (ROOT / "summary.json").write_text(json.dumps(out, indent=2) + "\n")
    try:
        import matplotlib.pyplot as plt
        names = list(out["runs"])
        vals = [out["runs"][n]["stats"]["gpu_ms"]["median_ms"] for n in names]
        fig, ax = plt.subplots(figsize=(7, 3.2), dpi=140)
        ax.bar(range(len(names)), vals, color="#4c78a8")
        ax.set_ylabel("GPU median (ms)"); ax.set_xticks(range(len(names)), names, rotation=45, ha="right")
        ax.set_title("D128 store ablations, motion frames 25–95")
        fig.tight_layout(); fig.savefig(ROOT / "gpu-median.png"); plt.close(fig)
    except Exception:
        pass

if __name__ == "__main__": main()
