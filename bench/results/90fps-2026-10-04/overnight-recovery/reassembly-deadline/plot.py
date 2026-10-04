import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
single=list(csv.DictReader((root/"single.csv").open()))
stereo=list(csv.DictReader((root/"stereo.csv").open()))
fig,axes=plt.subplots(1,2,figsize=(11,4.5))
for mode,label,color in [("skew3","Skew 3","#788296"),("2period","Two periods","#5623a6")]:
 rows=[r for r in single if r["policy"]==mode]
 axes[0].plot([int(r["source_fps"])for r in rows],[float(r["age_p90_ms"])for r in rows],"o-",label=label,color=color)
 rows=[r for r in stereo if r["scenario"]=="cross/"+mode]
 x=[int(r["source_fps"])for r in rows]
 axes[1].plot(x,[int(r["held_refreshes"])for r in rows],"o-",label=label,color=color)
for ax in axes:
 ax.set_xticks([30,60,90]);ax.set_xlabel("Source cadence (frames/s)");ax.spines[["top","right"]].set_visible(False);ax.legend()
axes[0].set_ylabel("Incomplete retirement age p90 (ms)");axes[0].set_title("Single-eye replay: earlier retirement")
axes[1].set_ylabel("Held display refreshes in finite replay");axes[1].set_title("Stereo repair race: one extra held refresh")
fig.suptitle("Earlier per-eye retirement can lose a coherent repair",fontsize=15)
fig.text(.5,.025,"Production reassembly + idealized stereo selector. Deterministic model; no live latency or FPS result.",ha="center",fontsize=9)
fig.tight_layout(rect=(0,.065,1,.93));fig.savefig(root/"tradeoff.png",dpi=160)
