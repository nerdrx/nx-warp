from pathlib import Path
import json,numpy as np
from PIL import Image
r=Path(__file__).resolve().parent;result={}
for variant in ['transport','transport-gated']:
 errors=[];progress=[]
 for i in range(32):
  d=r/'gpu-cap/full'/f'{i:03d}';paths={'held':d/'held.ppm','pred':r/variant/f'{i:03d}'/'warped.ppm','truth':d/'truth.ppm'};c={}
  for k,path in paths.items():
   a=np.array(Image.open(path),float);y,x=np.where((a[:,:,1]>1.4*a[:,:,0])&(a[:,:,1]>1.15*a[:,:,2])&(a[:,:,1]>35));c[k]=np.array([x.mean(),y.mean()]) if len(x)>=100 else None
  if any(v is None for v in c.values()):continue
  delta=c['truth']-c['held']
  if np.linalg.norm(delta)<4:continue
  errors.append(float(np.linalg.norm(c['pred']-c['truth'])));progress.append(float(np.dot(c['pred']-c['held'],delta)/np.dot(delta,delta)))
 result[variant]={'eligible':len(errors),'mean_centroid_error_px':float(np.mean(errors)),'median_projected_progress':float(np.median(progress))}
(r/'transport/alignment.json').write_text(json.dumps(result,indent=2));print(result)
