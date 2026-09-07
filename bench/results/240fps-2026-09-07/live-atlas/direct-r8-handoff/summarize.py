#!/usr/bin/env python3
"""Summarize recorded direct-R8 live windows; no frame-level percentile claims."""
import argparse,json,re,statistics
from pathlib import Path

def parse(p):
 t=Path(p).read_text(errors='replace')
 render=[(int(i),float(d),int(ns)) for i,d,ns in re.findall(r'render: (\d+) iterations in ([\d.]+) s.*?, (\d+) new-source',t)]
 # reorder above regex captures iterations, duration, new-source
 render=[{'iterations':i,'duration_s':d,'new_source':ns,'new_source_per_s':ns/d} for i,d,ns in render]
 gpu=[float(x) for x in re.findall(r'nxwarp\[\d+\] gpu duty ([\d.]+) ms/s',t)]
 copy=[float(x) for x in re.findall(r'copy(?: gpu)? ([\d.]+) ms',t,re.I)]
 own=[float(x) for x in re.findall(r"this app's own GPU pass ([\d.]+) ms",t)]
 direct=[int(x) for x in re.findall(r'atlas:.*?direct targets (\d+)',t)]
 return {'render_windows':render,'decoder_gpu_duty_ms_s':gpu,'copy_gpu_ms':copy,'render_gpu_ms':own,'direct_targets':direct}
ap=argparse.ArgumentParser();ap.add_argument('logs',nargs='+');ap.add_argument('--out',required=True);a=ap.parse_args();Path(a.out).write_text(json.dumps({Path(p).stem:parse(p) for p in a.logs},indent=2)+'\n')
