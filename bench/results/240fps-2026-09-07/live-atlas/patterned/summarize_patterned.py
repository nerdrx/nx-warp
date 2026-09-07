#!/usr/bin/env python3
"""Recompute matched patterned-run summaries using the parent parser."""
import importlib.util, json, re
from pathlib import Path
HERE=Path(__file__).resolve().parent
parent=HERE.parent/'summarize_live_atlas.py'
spec=importlib.util.spec_from_file_location('parent_summary',parent)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
out={}
for mode in ('off','auto'):
    s=HERE/f'{mode}-measure-filtered.log'; v=HERE/f'{mode}-server-filtered.log'
    d=mod.read_pair(s,v)
    t=[]
    for m in re.finditer(r'atlas: (\d+) frames, avg skip (\d+) intra (\d+) coded (\d+), atlas (\d+) picture (\d+)',v.read_text(errors='replace'),re.I):
        t.append({'frames':int(m[1]),'skip':int(m[2]),'intra':int(m[3]),'coded':int(m[4]),'atlas':int(m[5]),'picture':int(m[6])})
    d['server_atlas_reports']=t; out[mode]=d
Path(HERE/'summary-recomputed.json').write_text(json.dumps(out,indent=2)+'\n')
print('windows',out['off']['render_iterations']['count'],out['auto']['render_iterations']['count'],'atlas reports',len(out['auto']['server_atlas_reports']))
