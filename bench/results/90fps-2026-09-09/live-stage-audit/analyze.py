from pathlib import Path
import csv,json,importlib.util,statistics,gzip,argparse
p=Path(__file__).resolve().parent
parser=argparse.ArgumentParser();parser.add_argument('--csv',type=Path,default=p.parent/'live-latency/timings.csv.gz');args=parser.parse_args()
spec=importlib.util.spec_from_file_location('baseline',p.parent/'live-latency/summarize_pipeline_latency.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
opener=gzip.open if args.csv.suffix=='.gz' else open
with opener(args.csv,'rt') as f: rows=list(csv.reader(f));validated=m.summarize(rows,'nx');frames={}
for row in rows:
 if len(row)>=4 and row[3]=='0' and row[0] in ['receive_begin','receive_end','decode_begin','decode_end','blit']:
  e=frames.setdefault(int(row[1]),{});e[row[0]]=min(e.get(row[0],int(row[2])),int(row[2]))
cut=min(e['receive_begin'] for e in frames.values() if 'receive_begin' in e)+10e9
names=['receive_begin','receive_end','decode_begin','decode_end','blit'];cohort=[(f,e) for f,e in frames.items() if all(n in e for n in names) and e['receive_begin']>=cut];stages={};per=[]
for a,b in zip(names,names[1:]):
 vals=[(e[b]-e[a])/1e6 for _,e in cohort];assert all(v>=0 for v in vals),(a,b,min(vals));stages[a+' → '+b]={'count':len(vals),'mean_ms':statistics.mean(vals),'p50_p95_p99_ms':[m.percentile(vals,q) for q in [.5,.95,.99]]}
for f,e in cohort:per.append({'frame':f,**{a+'->'+b:(e[b]-e[a])/1e6 for a,b in zip(names,names[1:])}})
result={'source':'live-latency/timings.csv.gz (existing baseline; no new run)','baseline_validation':validated,'common_selected_cohort':len(cohort),'stages':stages};(p/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(stages,indent=2))
