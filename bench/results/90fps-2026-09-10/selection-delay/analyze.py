import json,re,sys
from pathlib import Path
rows=[]
for line in Path(sys.argv[1]).read_text().splitlines():
 m=re.search(r'selection-to-defoveate CPU ([\d.]+) ms mean \(max ([\d.]+)\) over (\d+) actual defoveate calls',line)
 if m: rows.append(dict(mean_ms=float(m[1]),max_ms=float(m[2]),calls=int(m[3])))
rows=rows[5:]
assert rows and all(r['calls']>0 for r in rows)
print(json.dumps({'excluded_initial_windows':5,'windows':len(rows),'calls':sum(r['calls'] for r in rows),'weighted_mean_ms':sum(r['mean_ms']*r['calls'] for r in rows)/sum(r['calls'] for r in rows),'max_ms':max(r['max_ms'] for r in rows),'rows':rows},indent=2))
