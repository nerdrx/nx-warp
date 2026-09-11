from pathlib import Path
import numpy as np,json
from PIL import Image
r=Path(__file__).resolve().parent;s=r.parent/'motion-scene'
def edge(path):
 im=np.asarray(Image.open(path).convert('RGB'),float)
 # Fixture-specific green-block silhouette; NEVER used by predictor.
 mask=(im[:,:,1]>1.4*im[:,:,0])&(im[:,:,1]>1.4*im[:,:,2])&(im[:,:,1]>40)
 rows=np.flatnonzero(mask.sum(axis=1)>=8)
 if len(rows)<30:return None
 low,high=rows[0],rows[-1];rows=np.arange(low+int(.2*(high-low)),high-int(.2*(high-low))+1)
 valid=mask[rows].sum(axis=1)>=8;coverage=float(valid.mean());rows=rows[valid]
 if len(rows)<10:return None
 edges=[np.argmax(mask[rows],axis=1),mask.shape[1]-1-np.argmax(mask[rows,::-1],axis=1)]
 vals=[]
 for x in edges:
  X=np.stack([rows,np.ones(len(rows))],axis=1);res=x-X@np.linalg.lstsq(X,x,rcond=None)[0];vals.append(float(np.percentile(np.abs(res),95)))
 return dict(bend_p95_px=max(vals),row_coverage=coverage)
allrows=[]
for idx in range(32):
 folders={'held':s/'gap2','truth':s/'gap2','grid8':s/'grid8','shared':r/'coherent','robust':r/'robust','hybrid':r/'hybrid'}
 row={'index':idx}
 for name,folder in folders.items():row[name]=edge(folder/f'{idx:03d}'/('held.ppm' if name=='held' else 'truth.ppm' if name=='truth' else 'warped.ppm'))
 allrows.append(row)
eligible=[x for x in allrows if x["truth"] and x["held"] and x["truth"]["bend_p95_px"]<=2 and x["held"]["bend_p95_px"]<=2 and x["truth"]["row_coverage"]==1 and x["held"]["row_coverage"]==1]
summary={}
for name in folders:
 vals=[x[name]['bend_p95_px'] for x in eligible if x[name]]
 summary[name]={'valid_frames':len(vals),'mean_bend_p95_px':float(np.mean(vals)),'worst_bend_p95_px':max(vals),'frames_above_2px':sum(x>2 for x in vals)}
(r/'edge-metrics.json').write_text(json.dumps(dict(summary=summary,eligible_indices=[x["index"] for x in eligible],frames=allrows),indent=2));print(json.dumps(summary,indent=2))
