from pathlib import Path
import csv,json,gzip,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).resolve().parent
def read(path):
 if path.exists():return path.read_bytes()
 return gzip.decompress(Path(str(path)+".gz").read_bytes())
def load(path):return list(csv.DictReader(read(path).decode().splitlines()))
rows={}
for mode in ('D','G'):
 for policy in ('service','paced'):
  data=load(root/f'{mode}-results/noisy-{policy}.csv')
  assert len(data)==61200
  rows[mode,policy]=data
  assert all(int(x['send_ns'])>0 for x in data)
  assert len({(x['scenario'],x['phase'],x['frame']) for x in data})==len(data)
summary=[]
for policy in ('service','paced'):
 d,g=rows['D',policy],rows['G',policy]
 for x,y in zip(d,g):
  assert all(x[k]==y[k] for k in x if k not in ('bytes','wire_ns','send_ns','bitrate_bps','estimate_bps'))
 for scenario in sorted({x['scenario'] for x in d}):
  for phase in ('0','360'):
   a=[x for x in d if x['scenario']==scenario and x['phase']==phase]
   b=[x for x in g if x['scenario']==scenario and x['phase']==phase]
   for mode,group in (('D',a),('G',b)):
    values=[int(x['bitrate_bps']) for x in group]
    summary.append(dict(policy=policy,scenario=scenario,phase=phase,mode=mode,min_bps=min(values),max_bps=max(values),final_bps=values[-1]))
 if policy=='paced':assert d==g,'Pacing-only control changed; inspect before reporting.'
cap=[]
for archived in sorted((root/'D-results/capacity').glob('*.csv*')):
 p=Path(str(archived).removesuffix('.gz'))
 q=root/'G-results/capacity'/p.name
 assert read(p)==read(q),p.name
 a=load(p)
 first=next((int(x['start_ns'])/1e9 for x in a if int(x['capacity_bps'])==1000000000 and int(x['bitrate_bps'])>=840000000),None)
 cap.append(dict(case=p.stem,rows=len(a),first_840_seconds=first,byte_exact=True))
D={x['case']:int(x['rate_bps']) for x in load(root/'D-results/span_cases.csv')}
G={x['case']:int(x['rate_bps']) for x in load(root/'G-results/span_cases.csv')}
expected={'one-fast-receive':266666667,'receive-slower':266666667,'serial-send-overlap-receive':266666667,'serial-receive-overlap-send':266666667,'partial-send':355555556,'send-gap':266666667,'unmatched-zero-byte-send':266666667,'reversed-receive':133333333,'duplicate-complete':266666667,'independent-origin-shift':266666667}
assert len(G)==19
for k,v in G.items():
 if k in expected:assert abs(v-expected[k])<1000,(k,v)
 else:assert v==D[k],(k,v,D[k])
with (root/'summary.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=summary[0].keys(),lineterminator="\n");w.writeheader();w.writerows(summary)
report={'noisy_rows_per_mode':122400,'capacity_byte_exact':cap,'focused_cases_pass':19,'pacing_only_all_rows_byte_exact':True,'limitations':['Virtual control traces, not throughput or photon tests.','Sender service and pacing regimes are models, not measured sender spans.','Loss-only bandwidth_estimate exposes controller fallback when no sample exists; not a wire sample.']}
(root/'gates.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
