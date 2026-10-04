import csv,statistics
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent
fig,axes=plt.subplots(2,2,figsize=(10,6))
for line,name,title in [(0,"encode","Warm recovery-blob encoding"),(1,"recovery","Single-erasure FEC reconstruction")]:
 rows=list(csv.DictReader((root/(name+".csv")).open()))
 for col,field,label in [(0,"mean_ns","Loop mean (ns/call)"),(1,"allocations","Allocation calls / operation")]:
  values=[statistics.median(float(r[field])for r in rows if r["treatment"]==mode)for mode in ["baseline","candidate"]]
  ax=axes[line,col];ax.bar(["Before","Reused metadata"],values,color=["#788296","#5623a6"],width=.5);ax.set_title(title);ax.set_ylabel(label);ax.set_ylim(0,max(values)*1.25);ax.spines[["top","right"]].set_visible(False)
  for x,v in enumerate(values):ax.text(x,v+max(values)*.025,f"{v:g}",ha="center")
fig.suptitle("Reuse bounded metadata; preserve wire bytes",fontsize=16)
fig.text(.5,.025,"Matched O2, CPU3, five ABBA blocks. Component loop means; no network/Pico/presentation timing.",ha="center",fontsize=9)
fig.tight_layout(rect=(0,.065,1,.93));fig.savefig(root/"comparison.png",dpi=160)
