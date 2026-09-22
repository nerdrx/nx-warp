from PIL import Image
import numpy as np
from pathlib import Path
import sys
p=Path(sys.argv[1]); repo=Path(sys.argv[2])
sources={'photo':repo/'bench/results/90fps-2026-09-11/motion-photo/source.png','scene':repo/'docs/assets/vrroom-mid.png'}
for name in ['photo','scene','noise']:
 data=[]
 for eye in range(2):
  if name=='noise': a=np.random.default_rng(42+eye).integers(0,256,(2176,2176,3),dtype=np.uint8).astype(np.float32)/255
  else:
   a=np.asarray(Image.open(sources[name]).convert('RGB').resize((2176,2176),Image.Resampling.LANCZOS),dtype=np.float32)/255
   if eye: a=np.roll(a,23,axis=1)
  y=a[:,:,0]*.2126+a[:,:,1]*.7152+a[:,:,2]*.0722
  cb=(a[:,:,2]-y)/1.8556+.5;cr=(a[:,:,0]-y)/1.5748+.5
  uv=np.stack([cb,cr],axis=-1).reshape(1088,2,1088,2,2).mean(axis=(1,3))
  data.append(np.round(y*255).clip(0,255).astype('uint8').tobytes()+np.round(uv*255).clip(0,255).astype('uint8').tobytes())
 (p/(name+'.nv12')).write_bytes(b''.join(data))

for name in ['colors','edges']:
 yy,xx=np.indices((2176,2176))
 if name=='colors':
  v=(xx+yy)//16%2;a=np.stack([1-v,v,np.zeros_like(v)],axis=-1).astype(np.float32)
 else:
  a=np.zeros((2176,2176,3),np.float32);a[:,:,0]=((xx//48)%2)*.9;a[:,:,1]=((yy//48)%2)*.8;a[:,:,2]=.25;a[(xx%96<5)|(yy%96<5)]=1
 y=a[:,:,0]*.2126+a[:,:,1]*.7152+a[:,:,2]*.0722
 cb=(a[:,:,2]-y)/1.8556+.5;cr=(a[:,:,0]-y)/1.5748+.5
 uv=np.stack([cb,cr],axis=-1).reshape(1088,2,1088,2,2).mean(axis=(1,3))
 b=np.round(y*255).astype('uint8').tobytes()+np.round(uv*255).clip(0,255).astype('uint8').tobytes()
 (p/(name+'.nv12')).write_bytes(b+b)
