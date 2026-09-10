from pathlib import Path
import json,subprocess,sys,re,statistics
p=Path(__file__).parent;root=Path(sys.argv[1]) if len(sys.argv)>1 else p.parent/'motion-live';labels=['base-a','candidate-a','candidate-b','base-b'];logs=[root/f'sha2-transport-{x}-client.log' for x in labels]
def get(name):return [json.loads(s) for s in subprocess.check_output([sys.executable,str(p/name),*map(str,logs)],text=True).splitlines()]
runs=get('analyze_warm.py');coverage=get('analyze_stability.py');budgets=[]
pat=re.compile(r'source->first ([-\d.]+) ms \| wire ([-\d.]+) \| queue ([-\d.]+) \| decode ([-\d.]+) \| decode->selection ([-\d.]+) \| selection->predicted ([-\d.]+) \| sum ([-\d.]+) over (\d+) selected frames \((\d+) invalid\)')
for path in logs:
 text=path.read_text();rows=[list(map(float,m.groups())) for m in pat.finditer(text)][5:];assert len(rows)>=20
 n=sum(r[7] for r in rows);assert n>0 and sum(r[8] for r in rows)==0
 means=[sum(r[i]*r[7] for r in rows)/n for i in range(7)]
 net=[float(v)*1000 for v in re.findall(r'net: ([\d.]+) ms per datagram',text)][5:]
 assert net
 budgets.append(dict(stages_ms=means[:6],total_ms=means[6],samples=n,windows=len(rows),net_us_per_datagram=statistics.mean(net)))
means={}
for mode,ii in [('base',[0,3]),('candidate',[1,2])]:
 means[mode]=dict(source_proxy_ms=statistics.mean(budgets[i]['total_ms'] for i in ii),packet_span_ms=statistics.mean(budgets[i]['stages_ms'][1] for i in ii),net_us_per_datagram=statistics.mean(budgets[i]['net_us_per_datagram'] for i in ii),decode_gpu_ms=statistics.mean(runs[i]['decoder']['means']['nxvc_gpu_ms'] for i in ii),fresh_per_covered_wall_second=statistics.mean(coverage[i]['fresh_per_covered_wall_second'] for i in ii))
(p/'live-summary.json').write_text(json.dumps(dict(runs=runs,coverage=coverage,budgets=budgets,means=means),indent=2)+'\n');print(json.dumps(means,indent=2))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
fig,axs=plt.subplots(2,2,figsize=(10,6),layout='constrained');names=['Base A','SHA2 A','SHA2 B','Base B'];colors=['#718096','#2583b5','#2583b5','#718096']
series=[('Receiver processing per datagram (µs)',[b['net_us_per_datagram'] for b in budgets]),('Packet arrival span (ms)',[b['stages_ms'][1] for b in budgets]),('Source-offset proxy (ms)',[b['total_ms'] for b in budgets]),('Fresh selections / covered wall-second',[r['fresh_per_covered_wall_second'] for r in coverage])]
for ax,(title,values) in zip(axs.flat,series):ax.bar(names,values,color=colors);ax.set_title(title);ax.grid(axis='y',alpha=.15);ax.set_axisbelow(True)
fig.suptitle('Hardware SHA-256 • 2688² per eye • four 60-second trials');fig.savefig(p/'live.png',dpi=150)
