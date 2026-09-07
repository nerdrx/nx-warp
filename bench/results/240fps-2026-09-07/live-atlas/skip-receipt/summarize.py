#!/usr/bin/env python3
"""Summarize one or more matched pace60 live captures; first window is warmup."""
import argparse, json, re, statistics
from pathlib import Path
NUM=r"(?:\d+(?:\.\d+)?|\.\d+)"
def med(xs): return statistics.median(xs) if xs else None
def parse(path, warmup):
 t=Path(path).read_text(errors='replace')
 win=[tuple(map(float,x)) for x in re.findall(r'nxwarp\[\d+\]: \d+ frames in .*? wall ('+NUM+r') ms, nxvc passA ('+NUM+r') passB ('+NUM+r') gpu ('+NUM+r') ms',t,re.I)][warmup:]
 src=[float(n)/float(s) for s,n in re.findall(r'render: \d+ iterations in ('+NUM+r') s.*?, (\d+) new-source',t,re.I)][warmup:]
 pose=[float(x) for x in re.findall(r'displayed pose age ('+NUM+r') ms mean',t,re.I)][warmup:]
 return {'windows':len(win),'decoder_window_means_ms':{'wall':med([x[0] for x in win]),'passA':med([x[1] for x in win]),'passB':med([x[2] for x in win]),'gpu':med([x[3] for x in win])},'fresh_source_per_s':med(src),'pose_age_ms_mean':med(pose),'warmup_windows_excluded':warmup}
def main():
 ap=argparse.ArgumentParser(); ap.add_argument('logs',nargs='+',type=Path); ap.add_argument('--out',type=Path,required=True); ap.add_argument('--warmup',type=int,default=0); a=ap.parse_args(); out={p.stem:parse(p,a.warmup) for p in a.logs}; a.out.write_text(json.dumps(out,indent=2)+'\n'); print(json.dumps(out,indent=2))
if __name__=='__main__': main()
