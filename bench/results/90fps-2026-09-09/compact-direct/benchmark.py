import subprocess,sys,json,re,hashlib
from pathlib import Path
import numpy as np
out=Path(__file__).resolve().parent
adb='/home/nerdrx/.local/bin/adb'
for i,variant in enumerate(sys.argv[1:] or ['baseline','direct','direct','baseline']):
 binary='/data/local/tmp/nx-compact-'+variant+'-vkdec'
 args=[adb,'shell','env','NXVC_VKD_PLANAR_FLAT=1',binary,'--in','/data/local/tmp/nx-compact-camera60.nxv','--no-out','--frames','60','--stats','--throughput','--format','ycbcr420','--unorm','0','--independent-tiles','--compact-centre']
 with (out/f'paired-{i}-{variant}.log').open('w') as f:subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,check=True)
 print(i,variant,'complete',flush=True)
summary={}
for variant in sorted(set(sys.argv[1:] or ['baseline','direct'])):
 values={k:[] for k in ['passA','passB','gpu','total']}
 for p in out.glob('paired-*-'+variant+'.log'):
  for line in p.read_text().splitlines():
   m=re.match(r'frame (\d+):',line)
   if not m or int(m[1])<10:continue
   for k in values:
    v=re.search(r'\b'+k+r' ([0-9.]+)',line)
    if v:values[k].append(float(v[1]))
 summary[variant]={k:{'n':len(v),'mean':float(np.mean(v)),'p50':float(np.percentile(v,50)),'p95':float(np.percentile(v,95)),'p99':float(np.percentile(v,99))} for k,v in values.items() if v}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary))
