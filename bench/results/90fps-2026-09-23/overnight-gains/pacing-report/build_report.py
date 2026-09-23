#!/usr/bin/env python3
from pathlib import Path
import csv
import matplotlib.pyplot as plt
out=Path(__file__).resolve().parent
rows=list(csv.DictReader((out/'runs.csv').open()))
for r in rows:
 for k in ('server_fps_mean','payload_mbps_mean','render_fps_mean','fresh_fps_mean','derived_delay_mean_ms','holes','tail_packets'): r[k]=float(r[k])
fig,axs=plt.subplots(1,3,figsize=(13,4.3),constrained_layout=True)
labels=[r['run'] for r in rows];x=list(range(len(rows)))
axs[0].bar(x,[r['fresh_fps_mean'] for r in rows],color='#377eb8');axs[0].set_xticks(x,labels,rotation=35,ha='right');axs[0].set_ylabel('fresh FPS');axs[0].set_title('Fresh source rate')
axs[1].bar(x,[r['derived_delay_mean_ms'] for r in rows],color='#e41a1c');axs[1].set_xticks(x,labels,rotation=35,ha='right');axs[1].set_ylabel('derived delay ms');axs[1].set_title('Software pipeline proxy')
axs[2].bar(x,[r['holes'] for r in rows],color='#55a868');axs[2].set_xticks(x,labels,rotation=35,ha='right');axs[2].set_ylabel('holes');axs[2].set_title('Reported holes')
fig.suptitle('Fixed pacing numeric comparison');fig.savefig(out/'comparison.png',dpi=160);fig.savefig(out/'comparison.svg');plt.close(fig)
