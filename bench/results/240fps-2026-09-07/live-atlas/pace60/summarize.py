#!/usr/bin/env python3
import argparse, json, re, statistics
from pathlib import Path

def stat(xs):
    if not xs: return {'count': 0, 'p50': None, 'p95': None}
    y=sorted(xs); x=.95*(len(y)-1); i=int(x)
    p=y[i] if i==len(y)-1 else y[i]+(y[i+1]-y[i])*(x-i)
    return {'count':len(y), 'p50':statistics.median(y), 'p95':p}

def parse(prefix):
    scene=Path(prefix+'-measure.log'); server=Path(prefix+'-server.log')
    s=scene.read_text(errors='replace'); v=server.read_text(errors='replace')
    def nums(p,t=s): return [float(x) for x in re.findall(p,t,re.I)]
    win=r'nxwarp\[\d+\]: \d+ frames in .*?'
    reports=re.findall(r'render: \d+ iterations in ([\d.]+) s.*?, (\d+) new-source',s,re.I)
    atlas=re.findall(r'atlas: frames (\d+), atlas (\d+), picture (\d+), avg dispatches ([\d.]+), avg assembled ([\d.]+), avg valid ([\d.]+)',s,re.I)
    return {'files':{'scene':str(scene),'server':str(server)},
      'new_source_per_s':stat([int(n)/float(d) for d,n in reports]),
      'decoder_gpu_ms':stat(nums(win+r'gpu ([\d.]+) ms')),
      'decoder_wall_ms':stat(nums(win+r'wall ([\d.]+) ms')),
      'passA_ms':stat(nums(win+r'passA ([\d.]+)')),
      'passB_ms':stat(nums(win+r'passB ([\d.]+)')),
      'copy_gpu_ms':stat(nums(r'fence-post [\d.]+ ms = nxvc gpu [\d.]+ \+ copy gpu ([\d.]+)')),
      'atlas_rows':[{'frames':int(a),'atlas':int(b),'picture':int(c),'dispatch':float(d),'assembled':float(e),'valid':float(f)} for a,b,c,d,e,f in atlas],
      'encoded_window_s':nums(r'encoded \d+ frames in ([\d.]+) s',v)}

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--off-prefix',required=True); ap.add_argument('--auto-prefix',required=True); ap.add_argument('--out',required=True); a=ap.parse_args()
    Path(a.out).write_text(json.dumps({'off_pace45':parse(a.off_prefix),'auto_pace60':parse(a.auto_prefix)},indent=2)+'\n')
if __name__=='__main__': main()
