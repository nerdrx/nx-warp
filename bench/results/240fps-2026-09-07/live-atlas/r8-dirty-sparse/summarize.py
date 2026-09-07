#!/usr/bin/env python3
import argparse,json,re,statistics
from pathlib import Path
N=r"(?:\d+(?:\.\d+)?|\.\d+)"
def one(p):
 t=Path(p).read_text(errors='replace'); w=[tuple(map(float,x)) for x in re.findall(r'nxwarp\[\d+\]: \d+ frames in .*? wall ('+N+r') ms, nxvc passA ('+N+r') passB ('+N+r') gpu ('+N+r') ms',t,re.I)]; src=[float(n)/float(s) for s,n in re.findall(r'render: \d+ iterations in ('+N+r') s.*?, (\d+) new-source',t,re.I)]; return {'decoder_windows':len(w),'gpu_ms':statistics.median([x[3] for x in w]) if w else None,'wall_ms':statistics.median([x[0] for x in w]) if w else None,'fresh_source_per_s':statistics.median(src) if src else None,'render_windows':len(src)}
a=argparse.ArgumentParser(); a.add_argument('logs',nargs='+',type=Path); a.add_argument('--out',required=True,type=Path); z=a.parse_args(); out={p.stem:one(p) for p in z.logs}; z.out.write_text(json.dumps(out,indent=2)+'\n'); print(json.dumps(out,indent=2))
