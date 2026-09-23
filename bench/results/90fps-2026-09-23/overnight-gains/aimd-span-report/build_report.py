#!/usr/bin/env python3
from pathlib import Path
import csv,matplotlib.pyplot as plt
out=Path(__file__).resolve().parent;runs=list(csv.DictReader((out/'runs.csv').open()));budget=list(csv.DictReader((out/'budget_windows.csv').open()));fresh=list(csv.DictReader((out/'fresh_windows.csv').open()))
for r in runs:
 for k in ('run','holes'):r[k]=int(r[k])
 for k in ('budget_mean_mbps','fresh_mean_fps'):r[k]=float(r[k])
for r in budget:r['window']=int(r['window']);r['run']=int(r['run']);r['budget_mbps']=float(r['budget_mbps'])
for r in fresh:r['window']=int(r['window']);r['run']=int(r['run']);r['fresh_fps']=float(r['fresh_fps'])
b2=[x for x in budget if x['run']==2];f2=[x for x in fresh if x['run']==2]
fig,axs=plt.subplots(2,2,figsize=(12,7),constrained_layout=True);axs[0,0].plot([x['window'] for x in b2],[x['budget_mbps'] for x in b2],color='#e41a1c');axs[0,0].set_title('B2 loss-only budget timeline');axs[0,0].set_ylabel('Direct planning budget (Mbit/s)');axs[0,0].set_xlabel('2 s window');axs[0,0].grid(alpha=.25);axs[0,1].plot([x['window'] for x in f2],[x['fresh_fps'] for x in f2],color='#e41a1c');axs[0,1].set_title('B2 freshness timeline');axs[0,1].set_ylabel('FPS');axs[0,1].set_xlabel('2 s window');axs[0,1].grid(alpha=.25);x=range(len(runs));axs[1,0].bar(x,[r['budget_mean_mbps'] for r in runs],color=['#377eb8' if r['case']=='A' else '#e41a1c' for r in runs]);axs[1,0].set_xticks(list(x),['A1 ordinary','B2 loss-only','B3 loss-only','A4 ordinary']);axs[1,0].set_title('Run budget means');axs[1,0].set_ylabel('Direct planning budget (Mbit/s)');axs[1,1].bar(x,[r['fresh_mean_fps'] for r in runs],color=['#377eb8' if r['case']=='A' else '#e41a1c' for r in runs]);axs[1,1].set_xticks(list(x),[f"{label} holes{r['holes']}" for label,r in zip(('A1 ordinary','B2 loss-only','B3 loss-only','A4 ordinary'),runs)]);axs[1,1].set_title('Run freshness / holes');axs[1,1].set_ylabel('FPS');fig.suptitle('AIMD: ordinary vs loss-only diagnostic');fig.savefig(out/'comparison.png',dpi=160);fig.savefig(out/'comparison.svg');plt.close(fig)
