from pathlib import Path
import time,json,shutil,subprocess
root=Path('/run/media/nerdrx/Lex/claude');live=root/'nx-scratch/motion-live';status=live/'large-thermal-soak-status.json'
while not status.exists(): time.sleep(10)
state=json.loads(status.read_text())
out=root/'nx-warp/bench/results/90fps-2026-09-11/large-centre-soak'
raw=out/'raw'
for suffix in ['client.log','server.log','scene.log','status.json']:
 p=live/('large-thermal-soak-'+suffix);shutil.copy2(p,raw/p.name)
data={}
for script in ['analyze_warm.py','analyze_stability.py']:
 val=subprocess.check_output(['python3',str(raw/script),str(raw/'large-thermal-soak-client.log')],text=True)
 (raw/('thermal-'+script+'.json')).write_text(val);data[script]=json.loads(val)
w=data['analyze_warm.py'];s=data['analyze_stability.py'];c=w['client']['means'];d=w['decoder']['means']
p=out/'README.md'
with p.open('a') as f:
 f.write(f"""
## Additional 25-minute stability run

Same retained profile, separate 1500-second run. Harness complete: **{state['complete']}**.
Fresh selections: **{s['fresh_per_covered_wall_second']:.2f}/covered wall-second** ({s['fresh_per_reported_second']:.2f}/reported second).
Decode GPU mean: **{d['nxvc_gpu_ms']:.3f} ms**, presentation GPU: **{c['own_gpu_ms']:.3f} ms**.
Source-offset proxy: **{c['source_offset_ms']:.2f} ms**.
Post-warm coverage: {s['covered_wall_seconds']:.2f} seconds; maximum summary gap: {s['max_window_gap_seconds']:.3f} seconds; logged session stops: {s['stopping_events']}.

The log label contains “thermal”; this is duration-based stability evidence, **not a temperature, power or thermal-throttling measurement**. It does not establish 90 fresh FPS. Raw logs and both analyses are included alongside the earlier run.
""")
print(json.dumps({'status':state,'warm':w,'stability':s},indent=2))
