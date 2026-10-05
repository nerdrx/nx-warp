import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
previous = root.parent / "fresh-peak-confirmation"
fig, axes = plt.subplots(2, 2, figsize=(12, 8), constrained_layout=True)
series = (("Baseline", previous / "raw/baseline", "#737784"), ("B: held", previous / "raw/confirmed-B", "#ed875f"), ("D: material bound", root / "raw", "#7541c5"))
for ax, case, step in ((axes[0,0], "rise3-clean", 3), (axes[0,1], "rise10-clean", 10), (axes[1,0], "rise20-clean", 20)):
    for label, folder, color in series:
        with (folder / "capacity" / (case + ".csv")).open() as f:
            data = [r for r in csv.DictReader(f) if (step-.5)*1e9 <= int(r["start_ns"]) <= (step+2)*1e9]
        ax.plot([int(r["start_ns"])/1e9-step for r in data], [int(r["bitrate_bps"])/1e6 for r in data], color=color, label=label)
    ax.axvline(0, color="#333333", linestyle=":")
    ax.set(title=f"500→1,000 Mbps rise at{step}s", xlabel="Virtual seconds after capacity rise", ylabel="Requested media target (Mbps)")
ax = axes[1,1]
for label, folder, color in series:
    with (folder / "probe-phase/rows.csv").open() as f:
        data = [r for r in csv.DictReader(f) if int(r["burst_offset"]) == 38]
    ax.plot([int(r["frame"])/90 for r in data], [int(r["bitrate_bps"])/1e6 for r in data], color=color, label=label)
ax.axhline(26.4, color="#333333", linestyle=":", label="Ordinary no-burst probe peak")
ax.set(title="Fast burst near probe:24 Mbps assumed link", xlabel="Virtual seconds after four-second warm-up", ylabel="Requested media target (Mbps)")
for ax in axes.flat:
    ax.grid(alpha=.2)
    ax.legend(fontsize=8)
fig.suptitle("Material divergence guard — actual controller, synthetic delivery\nRecovery and burst controls; no measured Wi-Fi or photon latency", fontsize=14)
fig.savefig(root / "material-bound.png", dpi=160)
fig.savefig(root / "material-bound.svg")
svg = root / "material-bound.svg"
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
