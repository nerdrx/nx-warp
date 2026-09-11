"""Causal merged-region tracker diagnostic; future is evaluation-only."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parent;sys.path.insert(0,str(r/'python-deps'))
import time,json,subprocess,cv2,numpy as np
from PIL import Image,ImageDraw,ImageFont
s=r.parent/'motion-scene';from temporal_regions import TemporalRegionTracker as Tracker
out=r/'tracked-merged';(out/'frames').mkdir(parents=True,exist_ok=True);cv2.setNumThreads(4);tracker=Tracker(hue_bins=8,min_area=180,merge_adjacent=True);font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19);rows=[]
for i in range(34):
 t0=time.perf_counter();cur=np.array(Image.open(s/'frames'/f'frame_{i+1:04d}.png').convert('RGB'));tracks=tracker.update(cur)
 if i<2: continue
 idx=i-2; moving=[t for t in tracks if t['confidence']>=.5 and 1<np.linalg.norm(t['velocity'])*2<80]; layers=[]; old=np.zeros((512,512),np.uint8); new=np.zeros_like(old)
 for t in moving:
  m=t['mask'];v=np.asarray(t['velocity'])*2;M=np.float32([[1,0,v[0]],[0,1,v[1]]]);a=cv2.warpAffine(m.astype(np.float32),M,(512,512),flags=cv2.INTER_LINEAR);col=cv2.warpAffine(cur,M,(512,512),flags=cv2.INTER_LINEAR);layers.append((m,a,col));old|=m;new|=(a>.01).astype(np.uint8)
 holes=old&(1-new);pred=cv2.inpaint(cur,holes*255,3,cv2.INPAINT_TELEA) if holes.any() else cur.copy()
 for m,a,col in sorted(layers,key=lambda x:-int(x[0].sum())): pred=np.uint8(np.clip(pred*(1-a[:,:,None])+col*a[:,:,None],0,255))
 truth=np.array(Image.open(s/'frames'/f'frame_{i+3:04d}.png').convert('RGB'));crop=np.s_[64:-64,64:-64,:];rm=lambda a:float(np.sqrt(np.mean((a[crop].astype(float)-truth[crop])**2)));ms=(time.perf_counter()-t0)*1000
 rows.append(dict(frame=idx,rmse=rm(pred),held_rmse=rm(cur),cpu_ms=ms,moving=len(moving),holes=int(holes.sum()),tracks=[dict(id=int(t['id']),centroid=np.asarray(t['centroid']).tolist(),velocity=np.asarray(t['velocity']).tolist(),confidence=float(t['confidence'])) for t in tracks]))
 im=Image.new('RGB',(1536,552),'#101725');dr=ImageDraw.Draw(im)
 for k,(a,title) in enumerate(((cur,'Held current'),(pred,'Merged tracked regions'),(truth,'Correct future'))): im.paste(Image.fromarray(a),(k*512,40));dr.text((k*512+8,8),title,font=font,fill='white')
 im.save(out/'frames'/f'{idx:03d}.png');print(idx,len(moving),round(rows[-1]['rmse'],2),flush=True)
(out/'scores.json').write_text(json.dumps(rows,indent=2));subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,345)) for p in sorted((out/'frames').glob('*.png'))[::2]];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0);print('means',{k:float(np.mean([v[k] for v in rows])) for k in ('rmse','held_rmse','cpu_ms','moving')})
