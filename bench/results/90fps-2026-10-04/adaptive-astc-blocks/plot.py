#!/usr/bin/env python3
from pathlib import Path
import csv
import matplotlib.pyplot as plt
p=Path(__file__).parent
rows=list(csv.DictReader((p/'metrics.csv').open()))
fig,ax=plt.subplots(figsize=(8.6,5.4),dpi=160)
colors={'dark':'#7c3aed','forest':'#16834a'}
for scene in ('dark','forest'):
 for metric,marker,ls,label in [('full_psnr_db','o','-','full frame'),('roi_psnr_db','s','--','512×512 ROI')]:
  ss=sorted((r for r in rows if r['scene']==scene),key=lambda r:int(r['lz4_payload_bytes']))
  ax.plot([int(r['lz4_payload_bytes'])/1000 for r in ss],[float(r[metric]) for r in ss],marker=marker,linestyle=ls,color=colors[scene],label=f'{scene}: {label}')
  for r in ss: ax.annotate(r['block'],(int(r['lz4_payload_bytes'])/1000,float(r[metric])),xytext=(4,5 if metric=='full_psnr_db' else -9),textcoords='offset points',fontsize=7)
ax.set_xlabel('LZ4 payload size (decimal kB)'); ax.set_ylabel('PSNR (dB)'); ax.set_title('ASTC block footprint: quality vs compressed bytes')
ax.grid(True,alpha=.25); ax.legend(fontsize=8,ncol=2); fig.tight_layout(); fig.savefig(p/'psnr-vs-lz4.png')
