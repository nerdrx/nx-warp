#!/usr/bin/env python3
"""Summarize the stale-PICTURE and reset captures; missing reports stay null."""
import argparse,json,re,statistics
from pathlib import Path
N=r"(?:\d+(?:\.\d+)?|\.\d+)"
def med(x): return statistics.median(x) if x else None
def one(scene):
 t=Path(scene).read_text(errors='replace'); w=[tuple(map(float,x)) for x in re.findall(r'nxwarp\[\d+\]: \d+ frames in .*? wall ('+N+r') ms, nxvc passA ('+N+r') passB ('+N+r') gpu ('+N+r') ms',t,re.I)]; src=[float(n)/float(s) for s,n in re.findall(r'render: \d+ iterations in ('+N+r') s.*?, (\d+) new-source',t,re.I)]; pose=[float(x) for x in re.findall(r'displayed pose age ('+N+r') ms mean',t,re.I)]; return {'decoder_windows':len(w),'decoder_gpu_ms':med([x[3] for x in w]),'decoder_wall_ms':med([x[0] for x in w]),'passA_ms':med([x[1] for x in w]),'passB_ms':med([x[2] for x in w]),'render_windows':len(src),'fresh_source_per_s':med(src),'pose_age_ms_mean':med(pose),'missing_render_or_pose_reports':not src or not pose}
a=argparse.ArgumentParser(); a.add_argument('--stale',required=True); a.add_argument('--reset',required=True); a.add_argument('--out',required=True,type=Path); z=a.parse_args(); out={'stale_flag':one(z.stale),'reset_flag':one(z.reset)}; z.out.write_text(json.dumps(out,indent=2)+'\n'); print(json.dumps(out,indent=2))
