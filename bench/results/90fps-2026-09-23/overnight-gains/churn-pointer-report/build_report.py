#!/usr/bin/env python3
from pathlib import Path
import csv,matplotlib.pyplot as plt
out=Path(__file__).resolve().parent;runs=list(csv.DictReader((out/'runs.csv').open()))
for r in runs:
 for k in ('payload_mbps','encode_ms','decode_ms','fresh_fps','holes'):r[k]=float(r[k])
fig,axs=plt.subplots(1,5,figsize=(16,4.2),constrained_layout=True)
for ax,key,title,y in ((axs[0],'payload_mbps','Payload','Mbps'),(axs[1],'encode_ms','Encode','ms'),(axs[2],'decode_ms','Decode','ms'),(axs[3],'fresh_fps','Fresh','FPS'),(axs[4],'holes','Holes','count')):
 x=range(len(runs));ax.bar(x,[r[key] for r in runs],color=['#377eb8' if i==0 else '#e41a1c' for i in x]);ax.set_xticks(list(x),[r['label'] for r in runs],rotation=25,ha='right');ax.set_title(title);ax.set_ylabel(y);ax.grid(axis='y',alpha=.25)
fig.suptitle('Partial churn pointer matrix: labeled runs only');fig.savefig(out/'comparison.png',dpi=160);fig.savefig(out/'comparison.svg');plt.close(fig)
