import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
fig,axes=plt.subplots(1,2,figsize=(9,4))
for run,ax in zip([1,2],axes):
 rows=list(csv.DictReader((root/f"run{run}.csv").open()))
 for idx,mode in enumerate(["copy_l1","direct_host_l1"]):
  v=sorted(float(r["wall_ms"])for r in rows if r["mode"]==mode)
  values=[v[int((len(v)-1)*p)]for p in [.5,.95]]
  ax.bar([idx*3,idx*3+1],values,color=["#788296","#5623a6"])
  for x,y in zip([idx*3,idx*3+1],values):ax.text(x,y+.035,f"{y:.3f}",ha="center",fontsize=9)
 ax.set_xticks([0,1,3,4],["Copy\np50","Copy\np95","Direct\np50","Direct\np95"]);ax.set_ylim(0,3.2);ax.set_ylabel("Offscreen paired wall time (ms)");ax.set_title(f"Matched ABBA run {run}");ax.spines[["top","right"]].set_visible(False)
fig.suptitle("Direct shader-to-host: small median gain, inconsistent tail")
fig.text(.5,.025,"50 measured pairs/mode/run. Native ASTC exact. Upload, transport and headset excluded.",ha="center",fontsize=9)
fig.tight_layout(rect=(0,.07,1,.92));fig.savefig(root/"comparison.png",dpi=160)
