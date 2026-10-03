#!/usr/bin/env python3
"""Rebuild DQ batching latency figure from copied run summaries and motion readback."""
from pathlib import Path
import re, csv
import numpy as np
import matplotlib.pyplot as plt
from PIL import Image, ImageDraw

ROOT=Path(__file__).parent
E=ROOT/'evidence'
def metric(filename, name):
    s=(E/filename).read_text()
    line=next(x for x in s.splitlines() if x.startswith('GPU='))
    m=re.search(rf'{name}=([0-9.]+)ms',line)
    return float(m.group(1))
# Equal-arm average for paired control/candidate/control; percent change is descriptive.
sets={
 'Paired DQ batching':('paired-dq-control1.log','paired-dq-batched.log','paired-dq-control2.log'),
 'Descriptor-write batching':('descriptor-control1.log','descriptor-candidate.log','descriptor-control2.log'),
 'Sign-prefix scan':None,
 'Host flush range':('flush-control1.log','flush-candidate.log','flush-control-repeat.log'),
}
data=[]
for name,files in sets.items():
    if files:
        c1,cand,c2=files
        for key in ('GPU_p50','GPU_p95'):
            a=metric(c1,key); b=metric(cand,key); c=metric(c2,key)
            base=(a+c)/2
            data.append((name,key,(b/base-1)*100,b,base))
    else:
        rows=list(csv.DictReader((E/'signscan-summary.csv').open()))
        d={r['variant']:r for r in rows}
        for key,col in [('GPU_p50','gpu_ms_p50'),('GPU_p95','gpu_ms_p95')]:
            a=float(d['control-pooled'][col]);b=float(d['signscan'][col])
            data.append((name,key,(b/a-1)*100,b,a))
with (ROOT/'latency-deltas.csv').open('w') as f:
    w=csv.writer(f,lineterminator="\n");w.writerow(['candidate','metric','percent_vs_control','candidate_ms','control_ms']);w.writerows(data)
order=['Paired DQ batching','Descriptor-write batching','Sign-prefix scan','Host flush range']
colors=['#2563eb','#9ca3af','#9ca3af','#9ca3af']
fig,(ax,ax2)=plt.subplots(1,2,figsize=(12.2,4.7),gridspec_kw={'width_ratios':[1.2,1]},layout='constrained')
y=np.arange(len(order)); offsets={'GPU_p50':-.13,'GPU_p95':.13}
for met,label,marker in [('GPU_p50','p50','o'),('GPU_p95','p95','s')]:
 vals=[next(d[2] for d in data if d[0]==name and d[1]==met) for name in order]
 ax.scatter(vals,y+offsets[met],s=62,marker=marker,label=label,color=colors,zorder=3)
 for x,yy in zip(vals,y+offsets[met]): ax.annotate(f'{x:+.1f}%',(x,yy),xytext=(7,0),textcoords='offset points',va='center',fontsize=9)
ax.axvline(0,color='#333',lw=1)
ax.set_yticks(y,order);ax.invert_yaxis();ax.set_xlabel('Latency change vs paired-control average (%)')
ax.set_title('Isolated candidate checks')
ax.grid(axis='x',color='#ddd',lw=.7);ax.legend(frameon=False,ncol=2,loc='upper center',bbox_to_anchor=(.5,-.12))
# Integrated full-root build: absolute Pico latency, 12 warmups + 30 measured samples.
labels=['Control\n(static, prior run)','Integrated\n(static)','Integrated\n(6 changing)']
p50=np.array([12.4661,12.1592,12.2250]);p95=np.array([12.9565,12.7095,12.7484])
pos=np.arange(3);width=.34
ax2.bar(pos-width/2,p50,width,label='p50',color='#2563eb')
ax2.bar(pos+width/2,p95,width,label='p95',color='#91b4e8')
ax2.axhline(1000/90,color='#c2410c',ls='--',lw=1.4,label='90 Hz budget')
ax2.set_xticks(pos,labels);ax2.set_ylabel('GPU decode latency (ms)');ax2.set_ylim(0,14.5)
ax2.set_title('Integrated build · Pico Adreno 650',fontsize=11)
ax2.legend(frameon=False,ncol=3,loc='upper center',bbox_to_anchor=(.5,-.12),fontsize=8)
ax2.grid(axis='y',color='#ddd',lw=.7)
for x,v in enumerate(p50):ax2.text(x-width/2,v+.12,f'{v:.2f}',ha='center',fontsize=8)
for x,v in enumerate(p95):ax2.text(x+width/2,v+.12,f'{v:.2f}',ha='center',fontsize=8)
fig.suptitle('Paired-DQ batching: modest GPU gain; full-image decode remains above 90 Hz budget',fontsize=12)
fig.savefig(ROOT/'dq-batching.png',dpi=180);fig.savefig(ROOT/'dq-batching.svg');plt.close(fig)
# Animation from actual host-decoded Y planes; downsample only for compact presentation.
motion=Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-motion420-20261003/decode')
W,H=4352,2176; frames=[]
shifts=[(0,0),(2,2),(4,2),(6,0),(4,-2),(2,-2)]
for i,(dx,dy) in enumerate(shifts):
    p=np.fromfile(motion/f'out.frame{i:03}.y.raw',dtype=np.uint8).reshape(H,W)
    im=Image.fromarray(p).resize((640,320),Image.Resampling.BOX).convert('RGB')
    dr=ImageDraw.Draw(im);dr.rounded_rectangle((8,8,202,34),radius=5,fill=(0,0,0));dr.text((15,14),f'Y only · synthetic shift {dx:+d},{dy:+d}',fill='white')
    frames.append(im)
frames[0].save(ROOT/'motion.png')
frames[0].save(ROOT/'motion.gif',save_all=True,append_images=frames[1:],duration=180,loop=0,optimize=True)
