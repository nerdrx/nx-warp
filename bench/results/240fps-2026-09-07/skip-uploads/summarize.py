#!/usr/bin/env python3
import argparse, glob, json, re, statistics
from pathlib import Path

def p95(xs):
    ys=sorted(xs); x=.95*(len(ys)-1); i=int(x)
    return ys[i] if i == len(ys)-1 else ys[i] + (ys[i+1]-ys[i])*(x-i)
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('directory',nargs='?',default='.'); ap.add_argument('--out',default='summary.json'); a=ap.parse_args(); d=Path(a.directory)
    result={}
    for arm_name in ('new','forced'):
        files=glob.glob(str(d/f'still-r8-{arm_name}-*.log')); g=[]; w=[]; rows=[]
        for f in sorted(files):
            gg=[]; ww=[]; dd=[]
            for l in Path(f).read_text(errors='replace').splitlines():
                m=re.match(r'frame (\d+):.*? (\d+) dispatches\).*? gpu ([0-9.]+).*? total ([0-9.]+)',l)
                if m and int(m[1])>0: dd.append(int(m[2])); gg.append(float(m[3])); ww.append(float(m[4]))
            if gg: rows.append({'file':Path(f).name,'warm_frames':len(gg),'dispatches':sorted(set(dd)),'gpu_ms':{'p50':statistics.median(gg),'p95':p95(gg)},'wall_ms':{'p50':statistics.median(ww),'p95':p95(ww)}}); g+=gg; w+=ww
        result[arm_name]={'runs':rows,'pooled_warm_frames':len(g),'gpu_ms':{'p50':statistics.median(g),'p95':p95(g)},'wall_ms':{'p50':statistics.median(w),'p95':p95(w)}}
    Path(a.out).write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__': main()
