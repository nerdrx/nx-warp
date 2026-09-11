from pathlib import Path
import json,re,subprocess,shutil
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';out=r/'nx-warp/bench/results/90fps-2026-09-11/hevc-motion-pose';raw=out/'raw';raw.mkdir(parents=True,exist_ok=True)
rows=[]
for label in ['hevc-pose-on','hevc-pose-off','hevc-pose-history']:
 p=live/(label+'-client.log');row=json.loads(subprocess.check_output(['python3',str(r/'nx-scratch/sha2-transport/analyze_warm.py'),str(p)],text=True));row['label']=label
 start=None;tot=dict(matched=0,active=0,applied=0,missing=0,unsafe=0);weighted=0
 for line in p.read_text(errors='replace').splitlines():
  tm=re.search(r'\d\d-\d\d (\d\d):(\d\d):(\d\d\.\d+)',line)
  if not tm:continue
  t=int(tm[1])*3600+int(tm[2])*60+float(tm[3])
  if 'render:' in line and 'iterations in' in line and start is None:start=t
  m=re.search(r'motion fields matched (\d+) active (\d+) pose-applied (\d+) pose-missing (\d+) pose-unsafe (\d+) mean active step ([\d.]+)',line)
  if m and start is not None and (t-start)%86400>=10:
   for k,v in zip(tot,m.groups()[:5]):tot[k]+=int(v)
   weighted+=int(m[2])*float(m[6])
 tot['mean_active_step']=weighted/tot['active'] if tot['active'] else 0;row['motion']=tot;rows.append(row)
 for suffix in ['client.log','server.log','scene.log','status.json']:shutil.copy2(live/(label+'-'+suffix),raw/(label+'-'+suffix))
(raw/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for name in ['run.py','run-history.py','report.py','apk-build.log','apk-history-build.log','math-test.log','trials.log','trials-history.log']:shutil.copy2(r/'nx-scratch/hevc-motion-pose'/name,raw/name)
for name in ['restart_server.py','capture_live.py']:shutil.copy2(live/name,raw/name)
shutil.copy2(r/'nx-scratch/sha2-transport/analyze_warm.py',raw/'analyze_warm.py')
for row in rows:print(row['label'],row['client'],row['motion'])
