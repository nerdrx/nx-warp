import json, re, statistics
from pathlib import Path
import matplotlib.pyplot as plt
root = Path(__file__).resolve().parent
flags = [int(x,16) for x in re.findall(r"flags 0x([0-9a-f]+)", (root.parent / "native-motion-stress/dense-d128.info").read_text())]
eligible = {i for i in range(1,len(flags)) if flags[i] & 32 and flags[i-1] & 32}
summary = {}
fig, ax = plt.subplots(figsize=(10,4), constrained_layout=True)
for file, label in [("pico-copy-off.log", "Full assembly control"), ("pico-copy-on.log", "Materialized copy"), ("pico-copy-on-repeat.log", "Copy repeat")]:
    rows = [(int(m[1]),float(m[2]),float(m[3])) for m in re.finditer(r"^frame (\d+):.*gpu ([\d.]+)  total ([\d.]+)", (root/file).read_text(), re.M)]
    assert len(rows) == 120
    summary[label] = {}
    for group, ids in [("eligible", eligible), ("motion", set(range(25,96)))]:
        picked = [r for r in rows if r[0] in ids]
        summary[label][group] = {"count": len(picked), "median_gpu_ms": statistics.median(r[1] for r in picked), "median_wall_ms": statistics.median(r[2] for r in picked)}
    ax.plot([r[0] for r in rows if r[0]>=25], [r[2] for r in rows if r[0]>=25], label=label, linewidth=1)
ax.axhline(1000/240, color="black", linestyle="--", label="4.17 ms target")
ax.set(xlabel="Source frame (startup omitted)", ylabel="Decode wall time (ms)", title="Pico 4: native dense synthetic pose stress, QP 40, threshold 128")
ax.legend(fontsize=8)
fig.savefig(root/"motion-timing.png", dpi=170)
(root/"summary.json").write_text(json.dumps(summary, indent=2)+"\n")
