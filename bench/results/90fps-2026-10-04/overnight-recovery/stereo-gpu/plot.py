from pathlib import Path
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
ROOT=Path(__file__).resolve().parent
rows=list(csv.DictReader((ROOT/"build/results/stereo.csv").open()))
fig,axes=plt.subplots(1,2,figsize=(11,4.7),layout="constrained")
for i,(mode,col) in enumerate([("serial","#8061ce"),("async","#38b5ac")]):
 r=[v for v in rows if v["mode"]==mode]
 vals=[float(v["wall_ms"]) for v in r];med=np.median(vals);p95=np.percentile(vals,95)
 axes[0].bar(i,med,color=col);axes[0].errorbar(i,med,yerr=[[0],[p95-med]],fmt="none",ecolor="#333",capsize=5)
 axes[0].text(i,med+.25,f"{med:.2f}",ha="center")
 for e in range(2):
  vals=[float(v[f"eye{e}_gpu_dispatch_ms"]) for v in r];m=np.median(vals);p=np.percentile(vals,95)
  x=e+(i-.5)*.32;axes[1].bar(x,m,.3,color=col,label=mode if not e else None)
  axes[1].errorbar(x,m,yerr=[[0],[p-m]],fmt="none",ecolor="#333",capsize=4)
axes[0].axhline(1000/90,color="#b34056",linestyle="--",label="90 Hz frame interval (11.11 ms)")
axes[0].set(title="Complete two-eye call, upload excluded",ylabel="CPU wall time (ms)",xticks=range(2),xticklabels=["Serial","Right async"],ylim=(0,18));axes[0].legend(fontsize=8)
axes[1].set(title="GPU shader time stays unchanged",ylabel="Dispatch timestamp duration (ms / eye)",xticks=range(2),xticklabels=["Dark eye","Forest eye"],ylim=(0,.75));axes[1].legend(fontsize=9)
fig.suptitle("Native 2176² eyes • one VkDevice/queue • median/p95 • NOT live viewer FPS",fontsize=12)
fig.savefig(ROOT/"stereo-gpu.png",dpi=170)
