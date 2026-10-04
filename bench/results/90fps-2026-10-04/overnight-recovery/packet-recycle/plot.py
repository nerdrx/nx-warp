from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
fig,axes=plt.subplots(1,2,figsize=(11,4.8))
fig.patch.set_facecolor("#100b1d")
for ax in axes:
 ax.set_facecolor("#100b1d");ax.tick_params(colors="white")
 for sp in ax.spines.values():sp.set_color("#827b98")
 ax.yaxis.label.set_color("white");ax.title.set_color("white")
axes[0].bar(["Baseline","Reuse"],[10000,20],color=["#827b98","#a16cff"])
axes[0].set_title("Vector capacity growths per 1,000 packets")
axes[0].set_ylabel("Growth count")
for x,y in enumerate([10000,20]):axes[0].text(x,y+180,f"{y:,}",ha="center",color="white")
axes[0].set_ylim(0,11400)
x=[0,1];width=.32
for offset,values,color,label in [(-width/2,[788.214,489.064],"#827b98","Baseline"),(width/2,[1.576428,.978128],"#a16cff","Reuse")]:
 bars=axes[1].bar([v+offset for v in x],values,width,color=color,label=label)
 for b,v in zip(bars,values):axes[1].text(b.get_x()+b.get_width()/2,v+15,f"{v:.2f}",ha="center",color="white",fontsize=9)
axes[1].set_xticks(x,["Dark fixture","Forest fixture"]);axes[1].set_title("Logical relocation bytes per 1,000 packets")
axes[1].set_ylabel("MB (decimal; estimated from vector growth)");axes[1].legend()
axes[1].set_ylim(0,900)
fig.text(.5,.025,"270 fragments/packet; startup included. Component counts only; no hardware memory-traffic, full decoder or Pico measurement.",ha="center",color="#c2b7d7",fontsize=8)
fig.tight_layout(rect=[0,.07,1,1]);fig.savefig(Path(__file__).with_name("packet-recycle.png"),dpi=180)
