from pathlib import Path
import subprocess,json,sys,re,statistics
p=Path(__file__).parent;root=Path(sys.argv[1]) if len(sys.argv)>1 else p.parent/'motion-live'
labels=['base-a','candidate-a','candidate-b','base-b'];logs=[root/f'sparse-layout-long-{x}-client.log' for x in labels]
def collect(script):return [json.loads(x) for x in subprocess.check_output([sys.executable,str(p/script),*map(str,logs)],text=True).splitlines()]
runs=collect('analyze_warm.py');coverage=collect('analyze_stability.py');temps=[]
for label,path in zip(labels,logs):
 vals=[float(x) for x in re.findall(r'GPUTemp=([0-9.]+)C',path.read_text()) if float(x)>0]
 assert vals
 temps.append(dict(label=label,first_C=vals[0],last_C=vals[-1],max_C=max(vals),valid_samples=len(vals)))
for r in runs:assert r['client']['n']>=50 and r['decoder']['n']>=50
means={}
for mode,indices in [('base',[0,3]),('candidate',[1,2])]:
 means[mode]={key:statistics.mean(runs[i]['decoder']['means'][key] for i in indices) for key in ['pass_a_ms','pass_b_ms','nxvc_gpu_ms','queue_ms','frames_per_s']}
 means[mode].update({key:statistics.mean(runs[i]['client']['means'][key] for i in indices) for key in ['own_gpu_ms','source_offset_ms']})
 means[mode]['fresh_per_covered_wall_second']=statistics.mean(coverage[i]['fresh_per_covered_wall_second'] for i in indices)
 means[mode]['fresh_per_reported_second']=statistics.mean(coverage[i]['fresh_per_reported_second'] for i in indices)
(p/'summary.json').write_text(json.dumps(dict(runs=runs,coverage=coverage,temperature=temps,means=means),indent=2)+'\n')
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
fig,axs=plt.subplots(2,2,figsize=(9,6),layout='constrained');names=['Base A','Static A','Static B','Base B'];colors=['#718096','#2583b5','#2583b5','#718096']
series=[('Pass A interval (ms)',[r['decoder']['means']['pass_a_ms'] for r in runs]),('Complete decode GPU (ms)',[r['decoder']['means']['nxvc_gpu_ms'] for r in runs]),('Source-offset proxy (ms)',[r['client']['means']['source_offset_ms'] for r in runs]),('Fresh selections / covered wall-second',[r['fresh_per_covered_wall_second'] for r in coverage])]
for ax,(title,vals) in zip(axs.flat,series):
 ax.bar(names,vals,color=colors);ax.set_title(title);ax.grid(axis='y',alpha=.15);ax.set_axisbelow(True)
fig.suptitle('2688² per eye • four 120-second Pico trials');fig.savefig(p/'sustained.png',dpi=150)
print(json.dumps(means,indent=2))
