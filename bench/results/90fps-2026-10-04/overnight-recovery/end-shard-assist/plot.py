import csv
from pathlib import Path
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
rows = list(csv.DictReader((root / "results.csv").open()))
tails = [1, 2, 16, 64, 128]
series = {}
for mode in ("baseline", "assist"):
    series[mode] = [
        100 * int(next(r for r in rows if r["mode"] == mode and int(r["tail_loss"]) == n)["recovered_tail"]) / n
        for n in tails
    ]

fig, ax = plt.subplots(figsize=(7.2, 4.4), layout="constrained")
xs = range(len(tails))
w = 0.36
ax.bar([x - w / 2 for x in xs], series["baseline"], w, label="Existing NACKs", color="#7895b2")
ax.bar([x + w / 2 for x in xs], series["assist"], w, label="Plus cached end shard", color="#ed9a55")
ax.set_xticks(list(xs), [str(n) for n in tails])
ax.set_xlabel("Consecutive tail data shards lost")
ax.set_ylabel("Lost tail shards recovered after at most 2 rounds (%)")
ax.set_ylim(0, 108)
ax.grid(axis="y", color="#cccccc", linewidth=0.6, alpha=0.7)
ax.set_axisbelow(True)
ax.legend(frameon=False, loc="upper left", bbox_to_anchor=(0, -0.18), ncol=2)
ax.set_title("Cached end-shard assist: deterministic source-class replay\nTail sweep omits parity; 2 rounds; 64 replies/request; not network timing", fontsize=11)
for x, pct in zip(xs, series["assist"]):
    ax.annotate(f"{pct:.1f}%", (x + w / 2, pct), xytext=(0, 3), textcoords="offset points",
                ha="center", fontsize=8)
fig.savefig(root / "recovery.png", dpi=180)
fig.savefig(root / "recovery.svg")

svg = root / "recovery.svg"
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
