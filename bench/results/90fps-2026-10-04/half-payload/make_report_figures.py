#!/usr/bin/env python3
"""Regenerate compact static-payload and app-owned GPU-pass figures."""
import csv, statistics
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np

ROOT=Path(__file__).resolve().parent
OUT=ROOT
scenes=['Dark 2176²','Forest 2176²','Crowd crop 2176×800']
data={
 'q6 LZ4': [501248,311230,151943],
 'q6 Zstd3': [416383,258375,116063],
 'q3 Zstd3': [288040,184734,100890],
 'q2 Zstd3': [243333,130834,92424],
}
base=np.array(data['q6 LZ4'],dtype=float)
fig,(ax,bx)=plt.subplots(1,2,figsize=(12,4.6),gridspec_kw={'width_ratios':[1.45,1]})
colors=['#64748b','#94a3b8','#f59e0b','#dc6b4a']
x=np.arange(3); w=.19
for j,(name,vals) in enumerate(data.items()):
 pct=np.array(vals)/base*100
 off=(j-1.5)*w
 bars=ax.bar(x+off,pct,w,label=name,color=colors[j])
 for bar,v in zip(bars,pct):
  ax.text(bar.get_x()+bar.get_width()/2,v+1.4,f'{v:.0f}%',ha='center',va='bottom',fontsize=8)
ax.axhline(50,color='#a33',linestyle='--',linewidth=1,label='half of q6 LZ4')
ax.set_ylim(0,125);ax.set_ylabel('Payload as % of same-scene q6 + LZ4')
ax.set_xticks(x,scenes);ax.set_title('Per-image payload (smaller is better)')
ax.grid(axis='y',alpha=.22);ax.legend(frameon=False,fontsize=8,ncol=2,loc='upper right')
rows=list(csv.DictReader((ROOT/'live-validation/presentation-gpu-pass.csv').open()))
for mode,marker,color,label in [(0,'o','#58748d','Mode 0'),(2,'s','#d66b4a','Mode 2')]:
 ys=[float(r['app_gpu_pass_ms']) for r in rows if int(r['mode'])==mode]
 xs=[mode+(k-(len(ys)-1)/2)*.09 for k in range(len(ys))]
 bx.scatter(xs,ys,s=46,marker=marker,color=color,label=label,zorder=3)
 bx.text(mode,max(ys)+.35,f'median {statistics.median(ys):.1f} ms\nrange {min(ys):.1f}–{max(ys):.1f}',ha='center',fontsize=8,color=color)
bx.axhline(11.11,color='#555',linestyle='--',linewidth=1,label='90 Hz frame interval')
bx.set_xlim(-.55,2.55);bx.set_ylim(0,12.3);bx.set_xticks([0,2],['Mode 0','Mode 2'])
bx.set_ylabel('App-owned GPU pass (ms / iteration)');bx.set_title('Headset stationary log windows')
bx.grid(axis='y',alpha=.22);bx.legend(frameon=False,fontsize=8,loc='upper right')
fig.suptitle('ASTC half-payload experiment and short live GPU check',y=1.03,fontsize=13)
fig.tight_layout()
fig.savefig(OUT/'report-summary.png',dpi=180,bbox_inches='tight')
plt.close(fig)
