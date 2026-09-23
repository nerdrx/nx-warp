#!/usr/bin/env python3
from pathlib import Path
import csv,json
import matplotlib.pyplot as plt
out=Path(__file__).resolve().parent
rows=list(csv.DictReader((out/'metrics.csv').open()))
for r in rows:
 r['run']=int(r['run']);r['mean']=float(r['mean'])
fig,axs=plt.subplots(2,2,figsize=(10,7),constrained_layout=True)
for ax,metric,title,ylabel in zip(axs.flat,('codec_wire_mbps','server_encode_ms','decode_ms','fresh_fps'),('Codec wire rate','Server encode','Client decode','Fresh frames'),('Mbps (codec payload)','ms','ms','FPS')):
 for case,color,label in (('A','#377eb8','A cache off'),('B','#e41a1c','B cache on')):
  rr=sorted((x for x in rows if x['case']==case and x['metric']==metric),key=lambda x:x['run'])
  ax.plot([x['run'] for x in rr],[x['mean'] for x in rr],'o-',color=color,label=label)
 ax.set_title(title);ax.set_xlabel('repeat');ax.set_ylabel(ylabel);ax.grid(alpha=.25)
axs[0,0].legend(fontsize=8);fig.suptitle('Live predictor repeat comparison (2 s windows)');fig.savefig(out/'comparison.png',dpi=160);fig.savefig(out/'comparison.svg');plt.close(fig)
