from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parent
fig,axes=plt.subplots(1,2,figsize=(12,4.9),layout="constrained")
data=[("Dark fixture",[1.0076,.9882,.9847,.9880],[1.0704,1.0595,1.0423,1.0460],[1.3147,1.0351,.9871,.9892],[1.3980,1.0972,1.0647,1.0444]),("Forest fixture",[.7436,.7269,.7257,.7253],[.7786,.7672,.7816,.8021],[.9748,.7605,.7245,.7310],[1.0590,.8189,.7380,.7728])]
x=np.arange(4);w=.35
for ax,(name,base,b95,cand,c95) in zip(axes,data):
 for off,label,color,med,p95 in [(-w/2,"Matched whole-frame","#8061ce",base,b95),(w/2,"Independent units","#38b5ac",cand,c95)]:
  bars=ax.bar(x+off,med,w,label=label,color=color)
  ax.errorbar(x+off,med,yerr=[np.zeros(4),np.array(p95)-med],fmt="none",ecolor="#333",capsize=3)
  ax.bar_label(bars,fmt="%.3f",padding=3,fontsize=8)
 ax.set(title=name,ylabel="CPU decode + writes per eye (ms)",ylim=(0,1.65),xticks=x,xticklabels=["256px\nsquares","256px\nbands","512px\nbands","1024px\nbands"])
 ax.legend(fontsize=8)
fig.suptitle("Native 2176² Pico • reusable contexts • median/p95 • no network or XR",fontsize=12)
fig.savefig(ROOT/"band-cpu.png",dpi=170)
