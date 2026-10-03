from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
root=Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003'); d=root/'motion-visual'; W,H=4352,2176
src=np.fromfile(root/'private-dark-native-stereo-444.yuv',dtype=np.uint8).reshape(3,H,W)
shifts=[(0,0),(1,0),(2,1),(1,2),(0,1),(-1,0)]; box=(1350,1320,1862,1832); frames=[]; metrics=[]
for i,(dx,dy) in enumerate(shifts):
 pad=[np.pad(pl,((3,3),(3,3)),mode='edge') for pl in src]
 raw=np.stack([pl[3+dy:3+dy+H,3+dx:3+dx+W] for pl in pad]); imgs=[]
 for name,prefix in [('Source',None),('CDF',f'out-cdf-{i:02}.frame000'),('Haar',f'out-haar-{i:02}.frame000')]:
  planes=raw if prefix is None else [np.fromfile(d/f'{prefix}.{s}.raw',dtype=np.uint8).reshape(H,W) for s in ('y','cb','cr')]
  y,cb,cr=[a.astype(np.float32) for a in planes]; cb-=128; cr-=128
  rgb=np.clip(np.stack((y+1.5748*cr,y-.187324*cb-.468124*cr,y+1.8556*cb),axis=2),0,255).astype(np.uint8)
  if prefix: metrics.append((i,name,*[float(np.abs(planes[j].astype(np.int16)-raw[j].astype(np.int16)).mean()) for j in range(3)]))
  imgs.append((name,Image.fromarray(rgb).crop(box).resize((512,512),Image.Resampling.LANCZOS)))
 canvas=Image.new('RGB',(1536,548),(14,17,23)); draw=ImageDraw.Draw(canvas)
 for j,(label,im) in enumerate(imgs): canvas.paste(im,(j*512,30)); draw.text((j*512+12,8),label,fill='white')
 draw.text((1370,8),f'{i+1}/6',fill=(190,200,210)); frames.append(canvas)
out=d/'paired-haar-motion-crop.gif'; frames[0].save(out,save_all=True,append_images=frames[1:],duration=250,loop=0,optimize=True)
frames[2].save(d/'paired-haar-motion-crop.png',optimize=True)
with (d/'plane-mae.csv').open('w') as f:
 f.write('frame,decoder,Y_MAE,Cb_MAE,Cr_MAE\n')
 for row in metrics:f.write(','.join(map(str,row))+'\n')
print(out,out.stat().st_size)
