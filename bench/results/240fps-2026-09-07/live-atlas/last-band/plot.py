#!/usr/bin/env python3
"""Plot all reported source-rate and pose-age windows in separate panels."""
import argparse,re
from pathlib import Path
import matplotlib.pyplot as plt
N=r"(?:\d+(?:\.\d+)?|\.\d+)"
def vals(p):
 t=Path(p).read_text(errors='replace')
 src=[float(n)/float(s) for s,n in re.findall(r'render: \d+ iterations in ('+N+r') s.*?, (\d+) new-source',t,re.I)]
 pose=[float(x) for x in re.findall(r'displayed pose age ('+N+r') ms mean',t,re.I)]
 return src,pose
def main():
 a=argparse.ArgumentParser(); a.add_argument('--before',required=True); a.add_argument('--after',required=True); a.add_argument('--reverse'); a.add_argument('--out',required=True); z=a.parse_args()
 cases=[('before',z.before,'#355c7d'),('before-reverse',z.reverse,'#6c8ebf'),('after',z.after,'#c06c84')]
 cases=[c for c in cases if c[1]]; fig,(top,bottom)=plt.subplots(2,1,sharex=True,figsize=(8,6.2),layout='constrained')
 for label,p,color in cases:
  src,pose=vals(p); x=range(1,len(src)+1); top.plot(x,src,'o-',label=label,color=color,linewidth=1.7,markersize=3); bottom.plot(x,pose,'o-',label=label,color=color,linewidth=1.7,markersize=3)
 top.set_ylabel('Fresh source / s'); top.grid(alpha=.25); bottom.set_ylabel('Pose age mean (ms)'); bottom.set_xlabel('Report window'); bottom.grid(alpha=.25); top.legend(ncol=len(cases),loc='lower center',bbox_to_anchor=(.5,1.01),frameon=False)
 fig.savefig(z.out+'.svg'); fig.savefig(z.out+'.png',dpi=180)
if __name__=='__main__':main()
