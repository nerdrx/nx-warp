#!/usr/bin/env python3
from pathlib import Path
import csv,matplotlib.pyplot as plt
out=Path(__file__).resolve().parent;runs=list(csv.DictReader((out/'runs.csv').open()));budget=list(csv.DictReader((out/'budget_windows.csv').open()))
for r in runs:
 for k in ('run','holes','safety_fallbacks_postcutoff'):r[k]=int(r[k])
 for k in ('payload_mean_mbps','fresh_mean_fps','derived_delay_mean_ms'):r[k]=float(r[k])
for r in budget:r['window']=int(r['window']);r['budget_mbps']=float(r['budget_mbps']);r['run']=int(r['run'])
fig,axs=plt.subplots(2,3,figsize=(14,7),constrained_layout=True)
for c,col,label in (('A','#377eb8','AIMD'),('B','#e41a1c','BBR')):
 rr=[r for r in budget if r['case']==c];axs[0,0].plot([r['window'] for r in rr],[r['budget_mbps'] for r in rr],color=col,label=label)
for ax,key,title,y in ((axs[0,1],'payload_mean_mbps','Payload mean','Mbps'),(axs[0,2],'fresh_mean_fps','Fresh mean','FPS'),(axs[1,0],'derived_delay_mean_ms','Derived software delay','ms'),(axs[1,1],'holes','Holes','count'),(axs[1,2],'safety_fallbacks_postcutoff','Safety fallbacks','count')):
 x=range(len(runs));ax.bar(x,[r[key] for r in runs],color=['#377eb8' if r['case']=='A' else '#e41a1c' for r in runs]);ax.set_xticks(list(x),[r['mode']+f" r{r['run']}" for r in runs]);ax.set_title(title);ax.set_ylabel(y);ax.grid(axis='y',alpha=.25)
axs[0,0].set_title('Actual codec budget per window');axs[0,0].set_ylabel('Mbps');axs[0,0].set_xlabel('2 s window');axs[0,0].legend();fig.suptitle('Controller check: AIMD vs BBR');fig.savefig(out/'comparison.png',dpi=160);fig.savefig(out/'comparison.svg');plt.close(fig)
