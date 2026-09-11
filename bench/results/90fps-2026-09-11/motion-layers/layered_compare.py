"""Offline image-only layered prediction. CPU diagnostic, not live GPU code.
Uses only previous/current pixels; future is loaded after prediction for scoring.
"""
from pathlib import Path
import sys,time,json,subprocess
sys.path.insert(0,str(Path(__file__).resolve().parent/'python-deps'))
import cv2,numpy as np
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;s=r.parent/'motion-scene';out=r/'layered';out.mkdir(exist_ok=True);(out/'frames').mkdir(exist_ok=True)
cv2.setNumThreads(4);cv2.setRNGSeed(314)
est=cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19)
rows=[]
for idx,i in enumerate(range(2,34)):
 t=time.perf_counter()
 prev=np.array(Image.open(s/'frames'/f'frame_{i-1:04d}.png').convert('RGB'));cur=np.array(Image.open(s/'frames'/f'frame_{i+1:04d}.png').convert('RGB'))
 gray=cv2.cvtColor(cur,cv2.COLOR_RGB2GRAY);pg=cv2.cvtColor(prev,cv2.COLOR_RGB2GRAY)
 back=est.calc(gray,pg,None);forward=est.calc(pg,gray,None)
 yy,xx=np.mgrid[:512,:512];qx=(xx+back[:,:,0]).astype(np.float32);qy=(yy+back[:,:,1]).astype(np.float32)
 rev=cv2.remap(forward,qx,qy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_CONSTANT,borderValue=1000)
 reliable=np.linalg.norm(back+rev,axis=2)<2
 hsv=cv2.cvtColor(cur,cv2.COLOR_RGB2HSV);h=hsv[:,:,0].astype(float);sat=hsv[:,:,1];val=hsv[:,:,2]
 objects=[]
 # Overlapping broad hue bands join differently lit faces without named object colours.
 for centre in range(0,180,15):
  dh=np.minimum(abs(h-centre),180-abs(h-centre));mask=((dh<12)&(sat>75)&(val>35)).astype(np.uint8)
  mask=cv2.morphologyEx(mask,cv2.MORPH_CLOSE,np.ones((3,3),np.uint8))
  n,lab,stats,_=cv2.connectedComponentsWithStats(mask)
  for k in range(1,n):
   area=int(stats[k,cv2.CC_STAT_AREA]);m=(lab==k).astype(np.uint8)
   if not 180<area<60000:continue
   if any(np.count_nonzero(m&o[0])/min(area,np.count_nonzero(o[0]))>.6 for o in objects):continue
   interior=cv2.erode(m,np.ones((5,5),np.uint8)).astype(bool)&reliable
   y,x=np.where(interior);x=x[::8];y=y[::8]
   if len(x)<12:continue
   a=np.stack([x,y],1).astype(np.float32);b=a+back[y,x]
   H,inliers=cv2.estimateAffinePartial2D(a,b,method=cv2.RANSAC,ransacReprojThreshold=2,maxIters=1000,confidence=.99)
   if H is None or inliers.mean()<.55:continue
   scale=np.linalg.norm(H[0,:2]);motion=np.median(np.linalg.norm(b-a,axis=1))
   if not .9<scale<1.1 or motion>85:continue
   objects.append((m,H,float(inliers.mean())))
 # Diagnostic background completion removes the retained silhouette before moving it.
 union=np.zeros((512,512),np.uint8)
 for m,H,c in objects:union|=m
 base=cv2.inpaint(cur,union*255,3,cv2.INPAINT_TELEA)
 prediction=base.copy()
 # Size ordering is a heuristic, NOT inferred scene depth.
 for m,H,c in sorted(objects,key=lambda o:-int(o[0].sum())):
  M=cv2.invertAffineTransform(H)
  colour=cv2.warpAffine(cur,M,(512,512),flags=cv2.INTER_LINEAR)
  alpha=cv2.warpAffine(m.astype(np.float32),M,(512,512),flags=cv2.INTER_LINEAR)
  prediction=np.uint8(np.clip(prediction*(1-alpha[:,:,None])+colour*alpha[:,:,None],0,255))
 elapsed=(time.perf_counter()-t)*1000
 # Future is evaluation-only; no truth-derived masks, fitting or ordering.
 truth=np.array(Image.open(s/'frames'/f'frame_{i+3:04d}.png').convert('RGB'))
 crop=np.s_[64:-64,64:-64,:]
 rmse=lambda a:float(np.sqrt(np.mean((a[crop].astype(float)-truth[crop])**2)))
 rows.append(dict(frame=idx,objects=len(objects),rmse=rmse(prediction),held_rmse=rmse(cur),cpu_ms=elapsed))
 im=Image.new('RGB',(1536,552),'#101725');dr=ImageDraw.Draw(im)
 for k,(a,title) in enumerate([(cur,'Held current'),(prediction,'Layered rigid + hole fill (CPU)'),(truth,'Correct future')]):
  im.paste(Image.fromarray(a),(512*k,40));dr.text((512*k+8,8),title,font=font,fill='white')
 im.save(out/'frames'/f'{idx:03d}.png')
 print(idx,len(objects),round(rows[-1]['rmse'],2),flush=True)
(out/'scores.json').write_text(json.dumps(rows,indent=2))
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,345)) for p in sorted((out/'frames').glob('*.png'))[::2]]
ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
print('means', {k:float(np.mean([v[k] for v in rows])) for k in ['rmse','held_rmse','cpu_ms','objects']})
