from pathlib import Path
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parent
fig,axes=plt.subplots(1,2,figsize=(11,4.5),layout="constrained")
for j,mode in enumerate(("legacy","compact")):
 rows=list(csv.DictReader((ROOT/f"results/current-{mode}.csv").open()))
 keys=["serial_batch_ms","persistent_batch_ms","async_batch_ms"]
 med=[np.median([float(r[k]) for r in rows]) for k in keys]
 p95=[np.percentile([float(r[k]) for r in rows],95) for k in keys]
 bars=axes[j].bar(range(3),med,color=["#8061ce","#4bb9ae","#f1ad61"])
 axes[j].errorbar(range(3),med,yerr=[np.zeros(3),np.array(p95)-med],fmt="none",ecolor="#333",capsize=5)
 axes[j].bar_label(bars,fmt="%.2f",padding=5)
 axes[j].set(title="Ordinary Zstd3" if not j else "Compact + ONE Zstd3 pass",ylabel="Two-eye batch wall time (ms)",ylim=(0,5.4),xticks=range(3),xticklabels=["Serial","Persistent","Right async"])
fig.suptitle("PC native 2176² eye packets • median/p95 • CPU probe, not live VR",fontsize=12)
fig.savefig(ROOT/"stereo-packing.png",dpi=170)
