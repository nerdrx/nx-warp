from pathlib import Path
import json,re,subprocess,shutil
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';out=r/'nx-warp/bench/results/90fps-2026-09-11/hevc-60-warp';raw=out/'raw';raw.mkdir(parents=True,exist_ok=True)
rows=[]
for label in ['hevc60-off-a','hevc60-on-a','hevc60-off-b','hevc60-v2-off-a','hevc60-v2-on-a','hevc60-v2-off-b']:
 p=live/(label+'-client.log');row=json.loads(subprocess.check_output(['python3',str(r/'nx-scratch/sha2-transport/analyze_warm.py'),str(p)],text=True));row['label']=label
 start=None;matched=active=0;step=0
 for line in p.read_text(errors='replace').splitlines():
  tm=re.search(r'\d\d-\d\d (\d\d):(\d\d):(\d\d\.\d+)',line)
  if not tm:continue
  t=int(tm[1])*3600+int(tm[2])*60+float(tm[3])
  if 'render:' in line and 'iterations in' in line and start is None:start=t
  m=re.search(r'motion fields matched (\d+) active (\d+) mean active step ([\d.]+)',line)
  if m and start is not None and (t-start)%86400>=10:
   matched+=int(m[1]);active+=int(m[2]);step+=int(m[2])*float(m[3])
 row['motion']={'matched':matched,'active':active,'mean_active_step':step/active if active else 0}
 gates=re.findall(r'source cap: admitted (\d+) skipped (\d+) in ([\d.]+) s', (live/(label+'-server.log')).read_text(errors='replace'))[5:]
 row['gate_admitted_per_s']=sum(int(x[0]) for x in gates)/sum(float(x[2]) for x in gates) if gates else None
 rows.append(row)
 for suffix in ['client.log','server.log','scene.log','status.json']:shutil.copy2(live/(label+'-'+suffix),raw/(label+'-'+suffix))
(raw/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for name in ['run.py','run-v2.py','report.py','build.log','build-v2.log','trials.log','trials-v2.log','admission-v1.patch']:shutil.copy2(r/'nx-scratch/hevc-60-warp'/name,raw/name)
for name in ['restart_server.py','capture_live.py']:shutil.copy2(live/name,raw/name)
shutil.copy2(r/'nx-scratch/sha2-transport/analyze_warm.py',raw/'analyze_warm.py')
shutil.copy2(r/'nx-scratch/hw-codec-probe/vaapi-hevc-2688-10bit.json',raw/'hevc-10bit.json')
for row in rows:print(row['label'],row['client'],row['motion'],row['gate_admitted_per_s'])
