import os,sys,subprocess,json,re
from pathlib import Path
import numpy as np
p=Path(__file__).resolve().parent;bins=Path(sys.argv[1]).resolve();w,h,n=4352,2176,32
y,x=np.indices((h,w));base=((x//16*13+y//16*7)%200+28).astype('uint8');c=np.full((h//2,w//2),128,dtype='uint8')
with (p/'large.yuv').open('wb') as f:
 for t in range(n):
  a=base.copy();a[700:1476,800:1376]=np.roll(base[700:1476,800:1376],t*13,axis=1);a[128:640,128+t*16:640+t*16]=220
  f.write(a.tobytes()+c.tobytes()+c.tobytes())
result=[]
for label,enabled in [('large-off',False),('large-on',True),('large-off-repeat',False)]:
 env=os.environ.copy();env.pop('NXVC_PLANAR_CADENCE',None);env['NXE_TIME']='1'
 if enabled:env.update(NXVC_PLANAR_CADENCE='1',NXVC_PLANAR_CADENCE_TRACE='1')
 with (p/(label+'.log')).open('w') as f:
  subprocess.run([str(bins/'nxvc-vkenc-api'),'--in',str(p/'large.yuv'),'--w',str(w),'--h',str(h),'--eyes','2','--out',str(p/(label+'.nxv')),'--frames',str(n),'--qp','40','--inter','--planar-gpu-centre','--centre-quarter','--centre-graduated'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
 text=(p/(label+'.log')).read_text();v=[float(x) for x in re.findall(r'gpu total ([0-9.]+) ms',text)][4:]
 rows=[list(map(int,m)) for m in re.findall(r'cadence: frame (\d+) fit (\d+) reuse (\d+) hot (\d+) max_age (\d+)',text)][4:]
 result.append({'label':label,'frames':len(v),'gpu_mean_ms':float(np.mean(v)),'gpu_p95_ms':float(np.percentile(v,95)),'peripheral_fit_mean':float(np.mean([r[1] for r in rows])) if rows else None,'peripheral_reuse_mean':float(np.mean([r[2] for r in rows])) if rows else None})
(p/'perf.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
