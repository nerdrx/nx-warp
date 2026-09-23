#!/usr/bin/env python3
from pathlib import Path
import csv,matplotlib.pyplot as plt
out=Path(__file__).resolve().parent;runs=list(csv.DictReader((out/'runs.csv').open()))
for r in runs:
 for k in ('run','ready_us','safety_fallbacks_postcutoff'):r[k]=int(r[k])
 for k in ('fresh_fps','derived_delay_ms','payload_mbps'):r[k]=float(r[k])
fig,axs=plt.subplots(1,4,figsize=(15,4.2),constrained_layout=True)
for ax,key,title,y in ((axs[0],'fresh_fps','Fresh FPS','FPS'),(axs[1],'derived_delay_ms','Derived delay','ms'),(axs[2],'payload_mbps','Payload','Mbps'),(axs[3],'safety_fallbacks_postcutoff','Safety fallbacks','count')):
 x=range(len(runs));ax.bar(x,[r[key] for r in runs],color=['#377eb8' if r['case']=='A' else '#e41a1c' for r in runs]);ax.set_xticks(list(x),[f"r{r['run']}\nready{r['ready_us']}" for r in runs]);ax.set_title(title);ax.set_ylabel(y);ax.grid(axis='y',alpha=.25)
fig.suptitle('Ready ABBA live comparison');fig.savefig(out/'comparison.png',dpi=160);fig.savefig(out/'comparison.svg');plt.close(fig)
