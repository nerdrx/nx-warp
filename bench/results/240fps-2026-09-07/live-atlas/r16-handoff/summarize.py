#!/usr/bin/env python3
import argparse,json,re,statistics
from pathlib import Path
N=r"(?:\d+(?:\.\d+)?|\.\d+)"
def one(p):
 t=Path(p).read_text(errors='replace'); w=[tuple(map(float,x)) for x in re.findall(r'nxwarp\[\d+\]: \d+ frames in .*? wall ([\d.]+) ms, nxvc passA ([\d.]+) passB ([\d.]+) gpu ([\d.]+) ms',t,re.I)]; c=[float(x) for x in re.findall(r'copy gpu ([\d.]+)',t,re.I)]; s=[int(n)/float(d) for d,n in re.findall(r'render: \d+ iterations in ([\d.]+) s.*?, (\d+) new-source',t,re.I)]; g=[float(x) for x in re.findall(r'own GPU pass ([\d.]+) ms',t,re.I)]; return {'decoder_windows':len(w),'decoder_gpu_ms':statistics.median([x[3] for x in w]) if w else None,'decoder_wall_ms':statistics.median([x[0] for x in w]) if w else None,'copy_gpu_ms':statistics.median(c) if c else None,'render_gpu_ms':statistics.median(g) if g else None,'active_source_per_s':statistics.median(s) if s else None}
a=argparse.ArgumentParser();a.add_argument('logs',nargs='+',type=Path);a.add_argument('--out',required=True,type=Path);z=a.parse_args();z.out.write_text(json.dumps({p.stem:one(p) for p in z.logs},indent=2)+'\n')
