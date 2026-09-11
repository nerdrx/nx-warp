"""Evaluation-only green centroid: never feeds the gate."""
from pathlib import Path
import numpy as np,json
from PIL import Image
r=Path(__file__).resolve().parent;rows=[]
def center(path):
 a=np.array(Image.open(path),float);m=(a[:,:,1]>1.4*a[:,:,0])&(a[:,:,1]>1.15*a[:,:,2])&(a[:,:,1]>35);y,x=np.where(m)
 return np.array([x.mean(),y.mean()]) if len(x)>=100 else None
for i in range(32):
 b=r/'gpu-cap/full'/f'{i:03d}';c={k:center(p) for k,p in {'held':b/'held.ppm','full':b/'warped.ppm','gate':r/'zero-preference/moving'/f'{i:03d}'/'warped.ppm','truth':b/'truth.ppm'}.items()}
 if any(v is None for v in c.values()):continue
 delta=c['truth']-c['held']
 if np.linalg.norm(delta)<4:continue
 row={'frame':i}
 for k in ['held','full','gate']:
  row[k+'_error']=float(np.linalg.norm(c[k]-c['truth']));row[k+'_progress']=float(np.dot(c[k]-c['held'],delta)/np.dot(delta,delta))
 rows.append(row)
summary={'eligible':len(rows),'mean_error':{k:float(np.mean([x[k+'_error'] for x in rows])) for k in ['held','full','gate']},'median_position_progress':{k:float(np.median([x[k+'_progress'] for x in rows])) for k in ['full','gate']}}
(r/'zero-preference/alignment.json').write_text(json.dumps({'summary':summary,'frames':rows},indent=2));print(summary)
