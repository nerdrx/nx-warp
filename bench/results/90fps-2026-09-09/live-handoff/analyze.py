from pathlib import Path
import argparse,re,json,statistics
p=Path(__file__).resolve().parent;ap=argparse.ArgumentParser();ap.add_argument('log',type=Path,nargs='?',default=p/'client.log');args=ap.parse_args()
pattern=r'\d\d-\d\d (\d\d:\d\d:\d\d\.\d+).*nxwarp\[0\] handoff frame (\d+): decode_gpu ([\d.]+) gap_gpu ([\d.]+) output_gpu ([\d.]+) ms'
rows=[]
for line in args.log.read_text().splitlines():
 m=re.search(pattern,line)
 if m:
  h,mi,se=map(float,m[1].split(':'));rows.append((h*3600+mi*60+se,int(m[2]),*map(float,m.groups()[2:])))
assert rows,'no active handoff measurements';assert all(rows[i][0]>=rows[i-1][0] for i in range(1,len(rows))),'time nonmonotonic (midnight unsupported)'
rows=[x for x in rows if x[0]>=rows[0][0]+10];assert len(rows)>1000
assert len({x[1] for x in rows})==len(rows),'duplicate frame IDs';assert all(0<=v<1000 for x in rows for v in x[2:]),'invalid timestamp span'
def pct(v,q):
 v=sorted(v);a=(len(v)-1)*q;i=int(a);return v[i]+(v[min(i+1,len(v)-1)]-v[i])*(a-i)
stages={name:{'mean_ms':statistics.mean(x[i] for x in rows),'p50_p95_p99_ms':[pct([x[i] for x in rows],q) for q in [.5,.95,.99]],'max_ms':max(x[i] for x in rows)} for i,name in enumerate(['decode','gap','output'],2)}
r={'warmup_s':10,'frames':len(rows),'observed_span_s':rows[-1][0]-rows[0][0],'gap_over_1ms':sum(x[3]>1 for x in rows),'stages':stages};(p/'results.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
