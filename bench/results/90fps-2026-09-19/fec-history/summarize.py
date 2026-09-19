from pathlib import Path
import re
import statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
rows = {}
for line in (root / "benchmark.txt").read_text().splitlines():
    if not line.startswith("round="):
        continue
    d = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
    rows.setdefault((d["fec"], d["mixed"]), []).append(
        ((d["old"] + d["old2"]) / 2e6, (d["new"] + d["new2"]) / 2e6))
labels, before, after = [], [], []
for (fec, mixed), values in rows.items():
    a, b = [statistics.median(x[i] for x in values) for i in (0, 1)]
    labels.append(f"FEC {'on' if fec else 'off'} / {'mixed' if mixed else 'primary'}")
    before.append(a)
    after.append(b)
    print(f"{labels[-1]}: {a:.3f} -> {b:.3f} us/16 shards; {(1-b/a)*100:.2f}% less")
fig, ax = plt.subplots(figsize=(8, 4.5))
x = list(range(len(labels)))
ax.bar([i-.19 for i in x], before, .38, label="Original", color="#999999")
ax.bar([i+.19 for i in x], after, .38, label="Reuse", color="#7700ff")
ax.set_xticks(x, labels, rotation=12)
ax.set_ylabel("CPU time (microseconds / 16 shards)")
ax.set_title("Recovery processing only — Ryzen 9 9950X3D")
ax.legend()
ax.spines[["top", "right"]].set_visible(False)
fig.text(.5, .015, "Median of 5 ABBA rounds; 1,000 batches/run. No network, codec or Pico timing.", ha="center", fontsize=9)
fig.tight_layout(rect=(0, .045, 1, 1))
fig.savefig(root / "recovery-cpu.png", dpi=150)
