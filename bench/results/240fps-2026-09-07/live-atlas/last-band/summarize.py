#!/usr/bin/env python3
"""Summarize completed last-band captures; medians use every measured window."""
import argparse,json,re,statistics
from pathlib import Path
N=r"(?:\d+(?:\.\d+)?|\.\d+)"
def med(x): return statistics.median(x) if x else None
def one(p):
 t=Path(p).read_text(errors='replace')
 w=[tuple(map(float,x)) for x in re.findall(r'nxwarp\[\d+\]: \d+ frames in .*? wall ('+N+r') ms, nxvc passA ('+N+r') passB ('+N+r') gpu ('+N+r') ms',t,re.I)]
 src=[float(n)/float(s) for s,n in re.findall(r'render: \d+ iterations in ('+N+r') s.*?, (\d+) new-source',t,re.I)]
 pose=[float(x) for x in re.findall(r'displayed pose age ('+N+r') ms mean',t,re.I)]
 return {'windows':len(w),'wall_ms':med([x[0] for x in w]),'passA_ms':med([x[1] for x in w]),'passB_ms':med([x[2] for x in w]),'gpu_ms':med([x[3] for x in w]),'fresh_source_per_s':med(src),'pose_age_ms_mean':med(pose)}
def main():
 a=argparse.ArgumentParser(); a.add_argument('logs',nargs='+',type=Path); a.add_argument('--out',required=True,type=Path); args=a.parse_args(); z={p.stem:one(p) for p in args.logs}; args.out.write_text(json.dumps(z,indent=2)+'\n'); print(json.dumps(z,indent=2))
if __name__=='__main__':main()
