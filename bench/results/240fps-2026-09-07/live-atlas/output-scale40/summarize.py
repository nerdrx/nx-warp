#!/usr/bin/env python3
import argparse,json,re,statistics
from pathlib import Path

def parse(p):
 t=Path(p).read_text(errors='replace')
 def med(pat):
  v=[float(x) for x in re.findall(pat,t,re.I)]; return statistics.median(v) if v else None
 return {'decoder_wall_ms':med(r'nxwarp\[\d+\]: \d+ frames in .*? wall ([\d.]+) ms'),
 'decoder_gpu_ms':med(r'nxwarp\[\d+\]: .*? gpu ([\d.]+) ms'),
 'copy_gpu_ms':med(r'copy gpu ([\d.]+) ms'),
 'render_gpu_ms':med(r"own GPU pass ([\d.]+) ms"),
 'render_windows':len(re.findall(r'render: \d+ iterations in [\d.]+ s',t)),
 'active_source_per_s': (statistics.median([int(n)/float(d) for d,n in re.findall(r'render: \d+ iterations in ([\d.]+) s.*?, (\d+) new-source',t,re.I)]) if re.findall(r'render: \d+ iterations in ([\d.]+) s.*?, (\d+) new-source',t,re.I) else None)}
ap=argparse.ArgumentParser();ap.add_argument('logs',nargs='+');ap.add_argument('--out',required=True);a=ap.parse_args();Path(a.out).write_text(json.dumps({Path(p).stem:parse(p) for p in a.logs},indent=2)+'\n')
