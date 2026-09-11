from pathlib import Path
import json,numpy as np
from PIL import Image
r=Path(__file__).resolve().parent

def centre(path):
 a=np.array(Image.open(path),float);y,x=np.where((a[:,:,1]>1.4*a[:,:,0])&(a[:,:,1]>1.15*a[:,:,2])&(a[:,:,1]>35));return np.array([x.mean(),y.mean()]) if len(x)>=100 else np.full(2,np.nan)
truth=np.array([centre(r/'gpu-cap/full'/f'{i:03d}'/'truth.ppm') for i in range(32)])
result={}
for name in ['held','full','cap','splat']:
 paths=[r/'forward-splat'/f'{i:03d}.png' if name=='splat' else r/'gpu-cap'/('cap' if name=='cap' else 'full')/f'{i:03d}'/('held.ppm' if name=='held' else 'warped.ppm') for i in range(32)]
 c=np.array([centre(p) for p in paths]);res=c-truth;v=np.linalg.norm(np.diff(res,axis=0),axis=1);acc=np.linalg.norm(np.diff(res,n=2,axis=0),axis=1)
 result[name]={'mean_residual_step_px':float(np.nanmean(v)),'p95_residual_step_px':float(np.nanpercentile(v,95)),'rms_residual_second_difference_px':float(np.sqrt(np.nanmean(acc**2))),'centres':c.tolist()}
(r/'forward-splat/temporal.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:{a:b for a,b in v.items() if a!='centres'} for k,v in result.items()},indent=2))
