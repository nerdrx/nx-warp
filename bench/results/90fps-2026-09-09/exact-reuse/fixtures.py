from pathlib import Path
import numpy as np,subprocess,os
p=Path(__file__).parent;w,h,n=4352,2176,16
y,x=np.indices((h,w),dtype=np.int32);a=((x*3+y*5+(x//17)*11)&255).astype('uint8');u=((x[::2,::2]*5+y[::2,::2]*3+67)&255).astype('uint8');v=((x[::2,::2]*2+y[::2,::2]*7+149)&255).astype('uint8')
for kind in ['static','motion']:
 with (p/(kind+'.yuv')).open('wb') as f:
  for t in range(n):
   for plane in [a,u,v]:
    z=plane
    if kind=='motion':
     ph,pw=z.shape;z=np.roll(z.reshape(ph,2,pw//2),(t*5,t*9),(0,2)).reshape(ph,pw)
    f.write(z.tobytes())
env=os.environ.copy();env.pop('NXVC_PLANAR_CADENCE',None);env['NXVC_PLANAR_WIDE_RING']='1'
for label,src,extra in [('static','static',[]),('motion','motion',[]),('qp','static',['--qp-cycle','40,40,30,30,40,40,26,26'])]:
 with (p/(label+'-encode.log')).open('w') as log:
  subprocess.run(['nx-warp/build-vk/bin/nxvc-vkenc-api','--in',str(p/(src+'.yuv')),'--out',str(p/(label+'.nxv')),'--w',str(w),'--h',str(h),'--eyes','2','--frames',str(n),'--qp','40','--inter','--planar-gpu-centre','--centre-quarter','--centre-graduated',*extra],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
