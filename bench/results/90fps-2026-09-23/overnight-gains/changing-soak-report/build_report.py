#!/usr/bin/env python3
from pathlib import Path
import csv,matplotlib.pyplot as plt
out=Path(__file__).resolve().parent;rows=list(csv.DictReader((out/'windows.csv').open()))
for r in rows:r['window']=int(r['window']);r['budget_mbps']=float(r['budget_mbps']);r['payload_mbps']=float(r['payload_mbps']);r['fresh_fps']=float(r['fresh_fps'])
x=[r['window'] for r in rows];fig,axs=plt.subplots(3,1,figsize=(12,8),sharex=True,constrained_layout=True);axs[0].plot(x,[r['budget_mbps'] for r in rows],color='#e41a1c');axs[0].set_ylabel('Planning budget Mbit/s');axs[0].set_title('Interrupted changing-loss-only soak');axs[1].plot(x,[r['payload_mbps'] for r in rows],color='#377eb8');axs[1].set_ylabel('Codec payload Mbit/s');axs[2].plot(x,[r['fresh_fps'] for r in rows],color='#55a868');axs[2].set_ylabel('Fresh FPS');axs[2].set_xlabel('2 s window');
for ax in axs:ax.grid(alpha=.25)
fig.savefig(out/'timeline.png',dpi=160);fig.savefig(out/'timeline.svg');plt.close(fig)
