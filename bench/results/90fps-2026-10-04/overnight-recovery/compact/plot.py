from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
ROOT=Path(__file__).resolve().parent
fig,axes=plt.subplots(1,2,figsize=(11,4.5),layout="constrained")
x=np.arange(2); width=.33
for off,label,color,values in [(-width/2,"Ordinary Zstd3","#8061ce",[416379,258371]),(width/2,"Compact + Zstd3","#38b5ac",[392362,245437])]:
 bars=axes[0].bar(x+off,np.array(values)/1000,width,label=label,color=color)
 axes[0].bar_label(bars,fmt="%.1f",padding=3,fontsize=9)
axes[0].set(title="Same ASTC texture, fewer payload bytes",ylabel="Compressed payload (kB, decimal)",xticks=x,xticklabels=["Dark room","Forest"],ylim=(0,475))
axes[0].legend(fontsize=9)
for off,label,color,p50,p95 in [(-width/2,"Ordinary Zstd3","#8061ce",[1.0249,.7606],[1.0941,.8251]),(width/2,"Compact in-place restore","#38b5ac",[1.0795,.8866],[1.1414,.9265])]:
 bars=axes[1].bar(x+off,p50,width,label=label,color=color)
 axes[1].errorbar(x+off,p50,yerr=[np.zeros(2),np.array(p95)-p50],fmt="none",ecolor="#333",capsize=4)
 axes[1].bar_label(bars,fmt="%.3f",padding=3,fontsize=9)
axes[1].set(title="Pico CPU decode: median with p95 whiskers",ylabel="Whole strict decode + restore (ms / eye)",xticks=x,xticklabels=["Dark room","Forest"],ylim=(0,1.32))
axes[1].legend(fontsize=8,loc="upper left")
fig.suptitle("Native 2176² per eye • exact independent packets • NOT live VR latency",fontsize=12)
fig.savefig(ROOT/"compact-results.png",dpi=170)
