#!/usr/bin/env python3
"""Parse per-eye ATLAS admission diagnostics from a server log."""
import argparse, json, re
from pathlib import Path

FIELDS = ('invalid', 'unconfirmed', 'aged', 'refresh', 'admitted', 'missing', 'no_ref', 'displacement')
PAT = re.compile(r'([LR]) invalid (\d+) unconfirmed (\d+) aged (\d+) refresh (\d+) admitted (\d+) missing (\d+) no-ref (\d+) displacement (\d+)', re.I)

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('server_log',nargs='?',default='live-admission-q40-pace45-server.log'); ap.add_argument('--out',default='admission-summary.json'); a=ap.parse_args()
    rows=[]
    for line in Path(a.server_log).read_text(errors='replace').splitlines():
        for m in PAT.finditer(line):
            vals=[int(x) for x in m.groups()[1:]]
            rows.append({'eye':m.group(1),'counts':dict(zip(FIELDS,vals))})
    aggregate={e:{f:sum(r['counts'][f] for r in rows if r['eye']==e) for f in FIELDS} for e in ('L','R')}
    Path(a.out).write_text(json.dumps({'source':str(a.server_log),'rows':rows,'aggregate':aggregate},indent=2)+'\n')
if __name__=='__main__': main()
