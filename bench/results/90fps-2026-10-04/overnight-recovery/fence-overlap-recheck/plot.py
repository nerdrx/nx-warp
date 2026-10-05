from pathlib import Path
import csv,json
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
r=list(csv.DictReader((p/"three-mode.csv").open()))
s=json.loads((p/"checked-summary.json").read_text())["summary"]
plt.rcParams.update({"font.size":11,"figure.facecolor":"#f8f7fc","axes.facecolor":"#f8f7fc"})
fig,axs=plt.subplots(1,2,figsize=(10.5,4.8))
for i in range(20):
 d={x["mode"]:float(x["wall_ms"]) for x in r[i*2:i*2+2]};axs[0].plot([0,1],[d["serial_l3"],d["parallel_l3"]],"o-",color="#7e57c2",alpha=.55,ms=4)
axs[0].set_xticks([0,1],["Serial","Parallel"]);axs[0].set_ylabel("Complete-call wall (ms)");axs[0].set_xlim(-.25,1.25);axs[0].set_title("20 matched pairs; every pair faster")
vals=[s[m]["wall_ms"][metric] for m in ["serial_l3","parallel_l3"] for metric in ["p50_lower_rank","p95_lower_rank"]]
labels=["Serial p50","Serial p95","Parallel p50","Parallel p95"]
axs[1].bar(labels,vals,color=["#273955","#273955","#51a6a6","#51a6a6"]);axs[1].set_ylabel("Complete-call wall (ms)");axs[1].set_ylim(0,9);axs[1].tick_params(axis="x",labelrotation=16)
for i,b in enumerate(vals):axs[1].text(i,b+.12,f"{b:.3f}",ha="center")
for ax in axs:ax.spines[["top","right"]].set_visible(False)
fig.suptitle("Same bytes, overlapped host packing: ~2.62 ms saved per pair",fontweight="bold")
fig.text(.5,.03,"Native2176²/slot • ordinary Zstd3 • identical shader/ASTC/packet bytes • GPU time unchanged\nOffscreen only; no live compositor, transport, Pico or photon measurement.",ha="center",fontsize=9)
fig.tight_layout(rect=(0,.12,1,.92));fig.savefig(p/"overlap.png",dpi=170)
