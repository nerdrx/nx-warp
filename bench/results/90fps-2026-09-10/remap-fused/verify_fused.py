"""Float32 test of remap plus compact mapping, including both eye clamps."""
from pathlib import Path
import json,numpy as np
F=np.float32;size=np.array([4352,2176],dtype='float32');target=np.array([1856,928],dtype='float32')
rng=np.random.default_rng(4);maxerr=0.;n=0
for eye in (0,1):
 centre=np.array([1088+eye*2176,1088],dtype='float32');origin=np.array([eye*2176,0],dtype='float32')
 axis=np.array([-2,.5,831.5,832,832.5,1343.5,1344,1344.5,2175.5,2178],dtype='float32')
 x,y=np.meshgrid(axis+origin[0],axis);p=np.concatenate([np.column_stack([x.ravel(),y.ravel()]),rng.uniform(origin-2,origin+2178,(200000,2)).astype('float32')])
 tile=(np.floor(p/F(64))+F(.5))*F(64);delta=tile-centre;r2=np.sum(delta*delta,axis=1,dtype='float32');mask=r2>F(65536)
 inv=np.where(r2<=F(333772.1072),F(.25),F(.125));phase=p*inv[:,None]-F(.5);phase-=np.floor(phase)
 stride=np.where(np.abs(p-centre)<F(256),F(1),F(4));filtered=p+(F(.5)-phase)*(F(1)/inv[:,None]-stride)
 q=np.where(mask[:,None],filtered,p);old=q/size*size;new=np.where(mask[:,None],q,old)
 def compact(q):
  local=np.clip(q-origin,F(.5),F(2175.5));mapped=local*F(.25)+np.clip(local-F(832),F(0),F(512))*F(.75)
  return (np.clip(mapped,F(.5),F(927.5))+np.array([eye*928,0],dtype='float32'))/target
 a,b=compact(old),compact(new);err=np.abs(a-b)*target;maxerr=max(maxerr,float(err.max()));n+=len(p)
 assert np.array_equal(a[~mask],b[~mask])
 assert np.isfinite(a).all() and np.isfinite(b).all()
assert maxerr<.001
out={'points':n,'dtype':'float32','both_eyes_and_borders':True,'max_compact_pixel_error':maxerr,'native_samples_identical':True,'scope':'Coordinate arithmetic only, not bit-exact GPU colour verification'}
Path(__file__).with_suffix('.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))
