from pathlib import Path
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
p=Path(__file__).resolve().parent
rows=list(csv.DictReader((p/"raw.csv").open()))
colors=["#ae90ee","#e48968","#7acfbd"]
fig,axes=plt.subplots(1,2,figsize=(11,4.8),facecolor="#171322")
for ax in axes:
    ax.set_facecolor("#211b2c");ax.tick_params(colors="white");ax.spines[['top','right']].set_visible(False)
    for s in ['left','bottom']:ax.spines[s].set_color('#8b7d9b')
    ax.set_xticks([0,1],["Fixture 0","Fixture 1"]);ax.grid(axis="y",alpha=.15,color="white");ax.set_axisbelow(True)
for j,(mode,label) in enumerate([('baseline','Baseline'),('candidate','All binary Q2'),('selected','Strict selector')]):
    x=np.arange(2)+(j-1)*.24
    error=[float(r[mode+'_mse'])/float(r['baseline_mse']) for r in rows]
    packet=[int(r[mode+'_packet'])/1000 for r in rows]
    axes[0].bar(x,error,.23,color=colors[j],label=label)
    axes[1].bar(x,packet,.23,color=colors[j],label=label)
    for xx,v in zip(x,error):axes[0].text(xx,v+.12,f'{v:.2f}×',ha='center',color='white',fontsize=9)
axes[0].set_ylabel('RGB MSE relative to baseline (lower better)',color='white');axes[0].set_ylim(0,9.8)
axes[1].set_ylabel('Zstd packet bytes / 1000 (lower better)',color='white')
fig.suptitle('Native production-paired gate: binary weights lose too much shading',color='white',fontsize=14)
handles,labels=axes[0].get_legend_handles_labels();fig.legend(handles,labels,loc='lower center',ncol=3,facecolor='#211b2c',labelcolor='white')
fig.tight_layout(rect=[0,.11,1,.91]);fig.savefig(p/'tradeoff.png',dpi=150,facecolor=fig.get_facecolor());plt.close(fig)
