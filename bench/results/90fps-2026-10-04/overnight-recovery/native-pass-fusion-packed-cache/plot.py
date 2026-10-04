from pathlib import Path
import csv,statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/"scope-alternating.csv").open()))
variants=["baseline","vec3cache","packedcache"];labels=["Two passes","Vector cache","Packed cache"]
means=[statistics.mean(float(r["astc_gpu_us"]) for r in rows if r["variant"]==v)/1000 for v in variants]
plt.rcParams.update({"font.size":11,"figure.facecolor":"#f8f7fc","axes.facecolor":"#f8f7fc"})
fig,axs=plt.subplots(1,2,figsize=(10,4.5))
axs[0].bar(labels,means,color=["#273955","#51a6a6","#7e57c2"]);axs[0].set_ylim(0,1.6);axs[0].set_ylabel("Mean ASTC GPU interval (ms)")
for i,b in enumerate(means):axs[0].text(i,b+.02,f"{b:.3f}",ha="center")
axs[1].bar(labels,[48,96,48],color=["#273955","#51a6a6","#7e57c2"]);axs[1].set_ylim(0,120);axs[1].set_ylabel("Private scratch (KiB / subgroup)")
for i,b in enumerate([48,96,48]):axs[1].text(i,b+2,str(b),ha="center")
for ax in axs:ax.spines[["top","right"]].set_visible(False);ax.tick_params(axis="x",labelrotation=12)
fig.suptitle("Packed cache: less private memory, slower shader",fontweight="bold")
fig.text(.5,.025,"2176² one eye • RX7900XTX • 12 rotated triples • Release host build\nAll ASTC bytes match; no live codec, network or Pico timing.",ha="center",fontsize=9)
fig.tight_layout(rect=(0,.10,1,.92));fig.savefig(p/"packed-cache.png",dpi=170)
