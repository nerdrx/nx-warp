from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
skew=[3,1,0]
threshold=[(n+1)*1000/90 for n in skew]
fig,ax=plt.subplots(figsize=(9,4.8))
fig.patch.set_facecolor("#100b1d");ax.set_facecolor("#100b1d")
bars=ax.barh(["Default: skew 3","Trial: skew 1","Trial: skew 0"],threshold,color=["#827b98","#9a63ff","#d2b6ff"])
for b,v in zip(bars,threshold):ax.text(v+.6,b.get_y()+b.get_height()/2,f"{v:.2f} ms",va="center",color="white")
ax.set_xlim(0,53);ax.invert_yaxis();ax.set_xlabel("Illustrative index threshold at 90 source updates/s",color="white")
ax.tick_params(colors="white");ax.spines[["top","right"]].set_visible(False)
for spine in ["left","bottom"]:ax.spines[spine].set_color("#827b98")
ax.set_title("Complete successors waiting behind an incomplete frame",color="white",pad=16)
fig.text(.5,.025,"Arithmetic only; not latency measurements. Shorter waits may forfeit repairs and reduce coherent stereo updates.",ha="center",color="#c2b7d7",fontsize=8)
fig.tight_layout(rect=[0,.075,1,1]);fig.savefig(Path(__file__).with_name("reassembly-threshold.png"),dpi=180)
