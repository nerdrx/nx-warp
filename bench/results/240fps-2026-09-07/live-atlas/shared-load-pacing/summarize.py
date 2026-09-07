#!/usr/bin/env python3
import argparse,json,re,statistics
from pathlib import Path
N=r"(?:\d+(?:\.\d+)?|\.\d+)"
def one(p):
 t=Path(p).read_text(errors='replace'); w=[float(x) for x in re.findall(r'nxwarp\[\d+\]: \d+ frames in .*? gpu ('+N+r') ms',t,re.I)]; src=[float(n)/float(s) for s,n in re.findall(r'render: \d+ iterations in ('+N+r') s.*?, (\d+) new-source',t,re.I)]; pose=[float(x) for x in re.findall(r'displayed pose age (-?'+N+r') ms mean',t,re.I)]; return {'decoder_windows':len(w),'decoder_gpu_ms_p50':statistics.median(w) if w else None,'active_source_per_s_p50':statistics.median(src) if src else None,'signed_display_value_p50':statistics.median(pose) if pose else None}
a=argparse.ArgumentParser(); a.add_argument('logs',nargs='+',type=Path); a.add_argument('--out',required=True,type=Path); z=a.parse_args(); out={p.stem:one(p) for p in z.logs}; z.out.write_text(json.dumps(out,indent=2)+'\n'); print(json.dumps(out,indent=2))
