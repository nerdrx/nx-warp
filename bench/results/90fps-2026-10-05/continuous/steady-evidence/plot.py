from pathlib import Path
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
D=root.parent/"noisy-feedback/raw/D/rows.csv"
F=root/"raw/F/rows.csv"
fig,axes=plt.subplots(2,2,figsize=(12,7),layout="constrained")
for ax,(case,phase,title) in zip(list(axes.flat)[:3],[("moderate-150",0,"Single 1.5× fast sample: steady phase"),("moderate-150",360,"Single 1.5× fast sample: near probe"),("repeated-150",360,"Repeated 1.5× anomalies every 23 frames")]):
    for mode,p,color in [("D",D,"#7c3aed"),("F",F,"#0284c7")]:
        with p.open() as f:rows=[r for r in csv.DictReader(f) if r["scenario"]==case and int(r["phase"])==phase]
        ax.step([int(r["elapsed_ns"])/1e9 for r in rows],[int(r["bitrate_bps"])/1e6 for r in rows],where="post",color=color,label=mode)
    ax.axhline(24,color="gray",ls="--",lw=1,label="Assumed capacity")
    ax.set(title=title,xlabel="Virtual time (s)",ylabel="Media target (Mbps)");ax.grid(alpha=.2);ax.legend()
ax=axes[1,1]
for mode,p,color in [("D",root.parent/"material-recovery-bound/raw/capacity/rise20-clean.csv","#7c3aed"),("F",root/"gates-F/capacity/rise20-clean.csv","#0284c7")]:
    with p.open() as f:rows=[r for r in csv.DictReader(f) if 19e9<=int(r["start_ns"])<=26e9]
    ax.step([int(r["start_ns"])/1e9 for r in rows],[int(r["bitrate_bps"])/1e6 for r in rows],where="post",color=color,label=mode)
ax.axvline(20,color="gray",ls="--",lw=1,label="500→1000 Mbps capacity")
ax.axhline(840,color="gray",ls=":",lw=1,label="Recovery threshold")
ax.set(title="Failing control: clean recovery delays 3.067 s",xlabel="Virtual sender time (s)",ylabel="Media target (Mbps)");ax.grid(alpha=.2);ax.legend()
fig.suptitle("Steady evidence guard F: cleaner noisy targets, slower clean recovery\nSynthetic controller replays; no fresh FPS or photon measurement",fontsize=13)
fig.savefig(root/"steady-evidence.png",dpi=170);fig.savefig(root/"steady-evidence.svg")
p=root/"steady-evidence.svg";p.write_text("\n".join(x.rstrip() for x in p.read_text().splitlines())+"\n")
