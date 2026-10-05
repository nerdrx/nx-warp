import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
colors = {"baseline": "#666b7a", "exploratory-A": "#ed8459", "confirmed-B": "#8345d6", "growth-bounded-C": "#039989"}
names = {"baseline": "Baseline", "exploratory-A": "A: unconfirmed (held)", "confirmed-B": "B: 12 samples (held)", "growth-bounded-C": "C: growth bound (held)"}
fig, axes = plt.subplots(2, 2, figsize=(12, 8), constrained_layout=True)
for ax, scenario, step in ((axes[0,0], "rise3-clean", 3), (axes[0,1], "rise10-clean", 10)):
    for mode, color in colors.items():
        with (root / "raw" / mode / "capacity" / (scenario + ".csv")).open() as f:
            rows = list(csv.DictReader(f))
        rows = [r for r in rows if (step-0.5)*1e9 <= int(r["start_ns"]) <= (step+2)*1e9]
        ax.plot([int(r["start_ns"])/1e9-step for r in rows], [int(r["bitrate_bps"])/1e6 for r in rows], color=color, label=names[mode])
    ax.axvline(0, color="#333333", linestyle=":")
    ax.set(title=f"500 → 1,000 Mbps capacity, rise at {step}s", xlabel="Virtual seconds after capacity rise", ylabel="Requested media target (Mbps)")
    ax.legend(fontsize=8)
ax = axes[1,0]
for mode, color in colors.items():
    with (root / "raw" / mode / "phase/rows.csv").open() as f:
        rows = [r for r in csv.DictReader(f) if int(r["burst_frame"]) == 8]
    ax.plot([int(r["frame"])/90 for r in rows], [int(r["bitrate_bps"])/1e6 for r in rows], color=color, label=names[mode])
ax.axhline(24, color="#333333", linestyle=":", label="Assumed link capacity")
ax.set(title="One fast burst on an unchanged 24 Mbps link", xlabel="Virtual seconds after steady warm-up", ylabel="Requested media target (Mbps)")
ax.legend(fontsize=8)
ax = axes[1,1]
for mode, color in colors.items():
    with (root / "raw" / mode / "phase/rows.csv").open() as f:
        rows = list(csv.DictReader(f))
    peaks = [max(int(r["bitrate_bps"])/1e6 for r in rows if int(r["burst_frame"]) == phase) for phase in range(47)]
    ax.plot(range(47), peaks, color=color, label=names[mode])
ax.axhline(24, color="#333333", linestyle=":")
ax.set(title="47 isolated-burst phases: A overshoots 20; B zero", xlabel="Burst frame offset", ylabel="Peak target within 3 virtual seconds (Mbps)")
ax.legend(fontsize=8)
for ax in axes.flat:
    ax.grid(alpha=.2)
fig.suptitle("Fresh-peak confirmation — actual controller, synthetic delivery\nNo measured network, headset or photon latency", fontsize=14)
fig.savefig(root / "fresh-peak.png", dpi=160)
fig.savefig(root / "fresh-peak.svg")

fig2, axs = plt.subplots(1, 3, figsize=(16, 5), constrained_layout=True)
for mode in ("baseline", "confirmed-B", "growth-bounded-C"):
    with (root / "raw" / mode / "probe-phase/rows.csv").open() as f:
        rows = list(csv.DictReader(f))
    chosen = [r for r in rows if int(r["burst_offset"]) == 38]
    axs[0].plot([int(r["frame"])/90 for r in chosen], [int(r["bitrate_bps"])/1e6 for r in chosen], color=colors[mode], label=names[mode])
    peaks = [max(int(r["bitrate_bps"])/1e6 for r in rows if int(r["burst_offset"]) == phase) for phase in range(47)]
    axs[1].plot(range(47), peaks, color=colors[mode], label=names[mode])
    with (root / "raw" / mode / "capacity/rise20-clean.csv").open() as f:
        rows = [r for r in csv.DictReader(f) if 19e9 <= int(r["start_ns"]) <= 25e9]
    axs[2].plot([int(r["start_ns"])/1e9-20 for r in rows], [int(r["bitrate_bps"])/1e6 for r in rows], color=colors[mode], label=names[mode])
axs[0].set(title="Burst near periodic probe: offset 38", xlabel="Virtual seconds after four-second warm-up", ylabel="Requested media target (Mbps)")
axs[1].set(title="Probe-phase burst peak: 47 offsets", xlabel="Burst frame offset", ylabel="Peak media target (Mbps)")
axs[2].set(title="C loses recovery speed: rise at 20s", xlabel="Virtual seconds after 500→1,000 Mbps rise", ylabel="Requested media target (Mbps)")
for ax in axs:
    ax.grid(alpha=.2)
    ax.legend(fontsize=8)
fig2.suptitle("Why all three candidates remain held — synthetic controller regressions", fontsize=14)
fig2.savefig(root / "held-gates.png", dpi=160)
fig2.savefig(root / "held-gates.svg")
for name in ("fresh-peak.svg", "held-gates.svg"):
    svg = root / name
    svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
