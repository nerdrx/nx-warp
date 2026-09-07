#!/usr/bin/env python3
import argparse,re
from pathlib import Path
import matplotlib.pyplot as plt
N=r"(?:\d+(?:\.\d+)?|\.\d+)"
def vals(p):
 t=Path(p).read_text(errors='replace'); g=[float(x) for x in re.findall(r'nxwarp\[\d+\]: \d+ frames in .*? gpu ('+N+r') ms',t,re.I)]; s=[float(n)/float(d) for d,n in re.findall(r'render: \d+ iterations in ('+N+r') s.*?, (\d+) new-source',t,re.I)]; return g,s
a=argparse.ArgumentParser(); a.add_argument('--full',required=True); a.add_argument('--dirty',required=True); a.add_argument('--reverse',required=True); a.add_argument('--out',required=True); z=a.parse_args(); fig,(u,v)=plt.subplots(2,1,figsize=(8,6),layout='constrained')
for l,p,c in [('full',z.full,'#355c7d'),('dirty',z.dirty,'#c06c84'),('full-reverse',z.reverse,'#6c8ebf')]:
 g,s=vals(p); u.plot(range(1,len(g)+1),g,'o-',label=l,color=c,ms=3); v.plot(range(1,len(s)+1),s,'o-',label=l,color=c,ms=3)
u.set_ylabel('Decoder GPU (ms)'); v.set_ylabel('Fresh source / s'); v.set_xlabel('Reported window'); u.grid(alpha=.25); v.grid(alpha=.25); u.legend(frameon=False,ncol=3,loc='lower center',bbox_to_anchor=(.5,1.01)); fig.savefig(z.out+'.svg'); fig.savefig(z.out+'.png',dpi=180)
