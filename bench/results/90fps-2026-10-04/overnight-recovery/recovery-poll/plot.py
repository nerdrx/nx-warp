from pathlib import Path
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
here=Path(__file__).resolve().parent
rows=list(csv.DictReader((here/"results.csv").open()))
assert len(rows)==80
plt.style.use("dark_background")
fig,axes=plt.subplots(1,2,figsize=(11,5),sharey=True)
for ax,n in zip(axes,[3,521]):
    for mode,label,color in [(0,"Arrival-only","#b7b3c6"),(1,"Quiet poll","#aa77ff")]:
        v=[float(r["first_request_opportunity_ms"]) for r in rows if int(r["shards"])==n and int(r["next_arrival"])==1 and int(r["adaptive"])==mode]
        assert len(v)==10 and min(v)>=0
        ax.scatter(np.linspace(mode-.08,mode+.08,len(v)),v,color=color,s=30,label=label)
        med,p95=np.percentile(v,[50,95]);ax.plot([mode-.15,mode+.15],[med,med],color=color,lw=3)
        ax.text(mode,med+1.3,f"p50 {med:.3f} ms",ha="center",fontsize=10,color=color)
        z=[float(r["first_request_opportunity_ms"]) for r in rows if int(r["shards"])==n and int(r["next_arrival"])==0 and int(r["adaptive"])==mode]
        assert len(z)==10
        if mode: assert min(z)>=0
        else: assert max(z)<0
    ax.set_xticks([0,1],["Arrival-only","Quiet poll"])
    ax.set_title(f"{n} shard metadata entries\nLater arrival signal at 20 ms")
    ax.set_ylim(0,25);ax.grid(axis="y",alpha=.2)
axes[0].set_ylabel("First repair-request opportunity (ms)")
fig.suptitle("An earlier repair opportunity when video goes quiet",fontsize=16)
fig.text(.5,.05,"No-signal condition: arrival-only never requests within 40 ms; quiet poll ~3.05 ms.\nHost pipe adapter + production helper/shard_set; excludes WiVRn, XR, Wi-Fi, Pico and photons.",ha="center",fontsize=10,color="#d0ccd9")
fig.tight_layout(rect=(0,.14,1,.93))
fig.savefig(here/"request-opportunity.png",dpi=160,facecolor="#100d18")
