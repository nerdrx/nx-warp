import json, numpy as np
from pathlib import Path
outdir=Path('/run/media/nerdrx/Lex/claude/nx-scratch/compact-clamp')
def mapped(p,eye,first):
 p=np.asarray(p,np.float32); base=np.array([eye*2176.,0.],np.float32); local=p-base
 if first: local=np.clip(local,np.float32(.5),np.float32(2175.5))
 m=local*.25+np.clip(local-np.float32(832.),np.float32(0),np.float32(512))*.75
 return np.clip(m,np.float32(.5),np.float32(927.5))
rng=np.random.default_rng(19); vals=np.array([-1e6,-2176,-1,-.5,0,.49,.5,2,831.5,832,832.5,1088,1344,2174,2175.5,2176,4352,1e6],np.float32); pts=[]
for eye in (0,1):
 for x in vals:
  for y in vals: pts.append((eye,np.array([x,y],np.float32)))
 for p in rng.uniform([-1e5,-1e5],[4352+1e5,1e5],(100000,2)).astype(np.float32): pts.append((eye,p))
maxerr=0.; diff=0
for eye,p in pts:
 e=np.max(np.abs(mapped(p,eye,True)-mapped(p,eye,False))); maxerr=max(maxerr,float(e)); diff+=int(e!=0)
r={'points':len(pts),'eyes':2,'boundary_grid':len(vals)**2*2,'max_mapped_float32_error':maxerr,'nonzero_differences':diff,'equivalent':diff==0}
(outdir/'verification.json').write_text(json.dumps(r,indent=2)+'\n'); print(json.dumps(r,indent=2))
