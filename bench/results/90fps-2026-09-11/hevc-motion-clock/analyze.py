from pathlib import Path
import re,json,statistics,subprocess
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live'
for label in ['hevc-clock','hevc-clock-control','hevc-clock-source']:
 p=live/(label+'-server.log')
 if not p.exists():continue
 rows=[(int(a),int(b),int(c),int(d),float(e)) for a,b,c,d,e in re.findall(r'motion clock: frame (\d+) new (\d+) app(?:_requested_display)? (\d+) compositor (\d+) delta_ms ([-\d.]+)',p.read_text(errors='replace'))]
 kept=[x for x in rows if x[1] and x[3]-rows[0][3]>=10e9] if rows else[];v=sorted(x[4] for x in kept)
 summary={'label':label,'app_vs_compositor':{'n':len(v),'mean_ms':statistics.mean(v),'p50_ms':statistics.median(v),'p95_ms':v[int(.95*(len(v)-1))]} if v else{}}
 p=live/(label+'-client.log');summary['render']=json.loads(subprocess.check_output(['python3',str(r/'nx-scratch/sha2-transport/analyze_warm.py'),str(p)],text=True))['client']
 start=None;tot=dict(matched=0,active=0,applied=0,missing=0,unsafe=0);weighted=0;source_applied=0
 for line in p.read_text(errors='replace').splitlines():
  tm=re.search(r'\d\d-\d\d (\d\d):(\d\d):(\d\d\.\d+)',line)
  if not tm:continue
  t=int(tm[1])*3600+int(tm[2])*60+float(tm[3])
  if 'render:' in line and 'iterations in' in line and start is None:start=t
  sm=re.search(r'source-clock pose shifts (\d+)',line)
  if sm and start is not None and (t-start)%86400>=10:source_applied+=int(sm[1])
  m=re.search(r'motion fields matched (\d+) active (\d+) pose-applied (\d+) pose-missing (\d+) pose-unsafe (\d+) mean active step ([\d.]+)',line)
  if m and start is not None and (t-start)%86400>=10:
   for k,value in zip(tot,m.groups()[:5]):tot[k]+=int(value)
   weighted+=int(m[2])*float(m[6])
 tot['mean_active_step']=weighted/tot['active'] if tot['active'] else 0;tot['source_clock_applied']=source_applied;summary['motion']=tot
 (r/'nx-scratch/hevc-motion-clock'/(label+'-summary.json')).write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
