from pathlib import Path
import subprocess,sys,json
p=Path(__file__).parent;live=Path(sys.argv[1]) if len(sys.argv)>1 else p
labels=['jit-cap-long-base-a','jit-cap-long-five-a','jit-cap-long-five-b','jit-cap-long-base-b']
rows=[]
for label in labels:
 s=json.loads((live/f'{label}-status.json').read_text());assert s['complete'] and s['client_alive']
 r=json.loads(subprocess.check_output([sys.executable,str(p/'analyze_warm.py'),str(live/f'{label}-client.log')],text=True));r['label']=label;r['mode']=(45000 if 'base' in label else 5000);rows.append(r)
avg={}
for mode in [45000,5000]:
 selected=[r for r in rows if r['mode']==mode]
 avg[str(mode)]={k:sum(r['client']['means'][k] for r in selected)/len(selected) for k in ['own_gpu_ms','fresh_per_s','source_offset_ms']}
out={'runs':rows,'mean_of_run_means':avg,'gpu_reduction_percent':100*(1-avg['5000']['own_gpu_ms']/avg['45000']['own_gpu_ms']),'scope':'Four 120-second synthetic moving-content trials, ABBA order; omit first 10 seconds of available telemetry. Active render windows only, not wall-clock FPS or photon latency.'}
(p/'summary.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({'means':avg,'gpu_reduction_percent':out['gpu_reduction_percent']}))
