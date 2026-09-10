from pathlib import Path
import subprocess,sys,json
p=Path(__file__).parent;live=Path(sys.argv[1]) if len(sys.argv)>1 else p
labels=['awake-wait-4a','awake-wait-1a','awake-wait-1b','awake-wait-4b']
rows=[]
for label in labels:
 s=json.loads((live/f'{label}-status.json').read_text());assert s['complete'] and s['client_alive']
 r=json.loads(subprocess.check_output([sys.executable,str(p/'analyze_warm.py'),str(live/f'{label}-client.log')],text=True));r['label']=label;r['mode']=int(s['wait_us']);rows.append(r)
avg={}
for mode in [4000,1000]:
 selected=[r for r in rows if r['mode']==mode]
 avg[str(mode)]={k:sum(r['client']['means'][k] for r in selected)/len(selected) for k in ['own_gpu_ms','fresh_per_s','source_offset_ms']}
out={'runs':rows,'mean_of_run_means':avg,'gpu_reduction_percent':100*(1-avg['1000']['own_gpu_ms']/avg['4000']['own_gpu_ms']),'scope':'Four 60-second synthetic moving-content trials, ABBA order; omit first 10 seconds of available telemetry. Active render windows only, not wall-clock FPS or photon latency.'}
(p/'summary.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({'means':avg,'gpu_reduction_percent':out['gpu_reduction_percent']}))
