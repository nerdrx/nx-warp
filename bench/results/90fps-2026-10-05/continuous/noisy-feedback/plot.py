import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
fig,axes=plt.subplots(2,2,figsize=(12,7),layout="constrained")
cases=[("moderate-150",360,"Single 1.5× fast sample near probe"),("record-anomalies-11",360,"Fast anomalies every 11 frames, tiny record increments"),("record-anomalies-11-collapse",0,"Record anomalies plus 24→12 Mbps collapse"),("gap-12-rise",360,"12 s callback pause, then 24→48 Mbps capacity")]
for ax,(case,phase,title) in zip(axes.flat,cases):
    for mode,color in [("baseline","#777777"),("D","#7c3aed"),("E","#0284c7")]:
        p=root/"raw"/mode/"rows.csv"
        if not p.exists():continue
        rows=[r for r in csv.DictReader(p.open()) if r["scenario"]==case and int(r["phase"])==phase]
        ax.step([int(r["elapsed_ns"])/1e9 for r in rows],[int(r["bitrate_bps"])/1e6 for r in rows],where="post",label=mode,color=color,lw=1.5)
    ax.set(title=title,xlabel="Virtual elapsed time (s)",ylabel="Requested media target (Mbps)")
    ax.grid(alpha=.2);ax.legend()
fig.suptitle("Noisy delivery feedback: actual controller, synthetic timestamps\nTargets are not throughput, fresh FPS or photon latency",fontsize=13)
fig.savefig(root/"noisy-feedback.png",dpi=170)
fig.savefig(root/"noisy-feedback.svg")
p=root/"noisy-feedback.svg";p.write_text("\n".join(x.rstrip() for x in p.read_text().splitlines())+"\n")
