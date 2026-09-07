#!/usr/bin/env python3
import argparse,re
from pathlib import Path
import matplotlib.pyplot as plt
N=r"(?:\d+(?:\.\d+)?|\.\d+)"
def vals(p):
 t=Path(p).read_text(errors='replace'); gpu=[float(x) for x in re.findall(r'nxwarp\[\d+\]: \d+ frames in .*? gpu ('+N+r') ms',t,re.I)]; src=[float(n)/float(s) for s,n in re.findall(r'render: \d+ iterations in ('+N+r') s.*?, (\d+) new-source',t,re.I)]; return gpu,src
a=argparse.ArgumentParser(); a.add_argument('--stale',required=True); a.add_argument('--reset',required=True); a.add_argument('--out',required=True); z=a.parse_args(); fig,(top,bot)=plt.subplots(2,1,sharex=False,figsize=(8,6),layout='constrained')
for label,p,c in [('stale_flag',z.stale,'#355c7d'),('reset_flag',z.reset,'#c06c84')]:
 g,s=vals(p); top.plot(range(1,len(g)+1),g,'o-',label=label,color=c,ms=3); bot.plot(range(1,len(s)+1),s,'o-',label=label,color=c,ms=3)
top.set_title('PICTURE flag reset capture'); top.set_ylabel('Decoder GPU (ms)'); bot.set_ylabel('Fresh source / s'); bot.set_xlabel('Reported window'); top.grid(alpha=.25); bot.grid(alpha=.25); top.legend(frameon=False,ncol=2,loc='lower center',bbox_to_anchor=(.5,1.01)); fig.savefig(z.out+'.svg'); fig.savefig(z.out+'.png',dpi=180)
