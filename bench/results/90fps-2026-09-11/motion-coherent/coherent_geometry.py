"""Analytic bilinear-field Jacobian diagnostic, not a silhouette-quality test."""
from pathlib import Path
import json,numpy as np
r=Path(__file__).resolve().parent/'coherent-field';y,x=np.mgrid[64:448,64:448];p=np.stack([(x+.5)/512,(y+.5)/512],-1);g=p*64-.5;c=np.floor(g).astype(int);f=g-c;cx=c[:,:,0];cy=c[:,:,1];rows=[]
for i in range(32):
 a=np.fromfile(r.parent/'gpu-cap/full'/f'{i:03d}'/'field.f32',np.float32).reshape(2,64,64,2)
 b=np.fromfile(r/'moving'/f'{i:03d}'/'applied.f32',np.float32).reshape(2,64,64,2)
 results={}
 for name,field in [('full',a),('clean',b)]:
  v00=field[:,cy,cx];v10=field[:,cy,cx+1];v01=field[:,cy+1,cx];v11=field[:,cy+1,cx+1]
  fx=f[:,:,0,None];fy=f[:,:,1,None];d=(v00*(1-fx)+v10*fx)*(1-fy)+(v01*(1-fx)+v11*fx)*fy
  dx=((v10-v00)*(1-fy)+(v11-v01)*fy)*64;dy=((v01-v00)*(1-fx)+(v11-v10)*fx)*64
  results[name]=((1-dx[:,:,:,0])*(1-dy[:,:,:,1])-dy[:,:,:,0]*dx[:,:,:,1],np.all((p-d>0)&(p-d<1),axis=-1))
 valid=results['full'][1]&results['clean'][1];row={'frame':i,'common_unclamped_samples':int(valid.sum())}
 for name,(det,_) in results.items():row[name+'_nonpositive_jacobian_fraction']=float((det[valid]<=0).mean())
 rows.append(row)
(r/'geometry.json').write_text(json.dumps(rows,indent=2));print({name:sum(x[name+'_nonpositive_jacobian_fraction'] for x in rows)/32 for name in ['full','clean']})
