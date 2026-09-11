from pathlib import Path
import json,numpy as np
from PIL import Image
r=Path(__file__).resolve().parent;results=[]
for red,blue in [(1.3,1.1),(1.4,1.15),(1.5,1.2)]:
 errors={k:[] for k in ['held','full','cap']};progress={k:[] for k in ['full','cap']}
 for i in range(32):
  d=r/'gpu-cap/full'/f'{i:03d}';paths={'held':d/'held.ppm','full':d/'warped.ppm','cap':r/'gpu-cap/cap'/f'{i:03d}'/'warped.ppm','truth':d/'truth.ppm'};c={}
  for k,p in paths.items():
   a=np.array(Image.open(p),dtype=float);y,x=np.where((a[:,:,1]>red*a[:,:,0])&(a[:,:,1]>blue*a[:,:,2])&(a[:,:,1]>35));c[k]=np.array([x.mean(),y.mean()]) if len(x)>=100 else None
  if any(v is None for v in c.values()):continue
  delta=c['truth']-c['held'];dist=np.linalg.norm(delta)
  if dist<4:continue
  for k in errors:errors[k].append(float(np.linalg.norm(c[k]-c['truth'])))
  for k in progress:progress[k].append(float(np.dot(c[k]-c['held'],delta)/np.dot(delta,delta)))
 results.append({'red_ratio':red,'blue_ratio':blue,'eligible':len(errors['held']),'mean_error':{k:float(np.mean(v)) for k,v in errors.items()},'median_progress':{k:float(np.median(v)) for k,v in progress.items()}})
(r/'alignment/sensitivity.json').write_text(json.dumps(results,indent=2));print(results)
