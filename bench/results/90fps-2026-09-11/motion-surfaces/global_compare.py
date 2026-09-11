from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'python-deps'))
import cv2,numpy as np,subprocess,os,json,re
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;s=r.parent/'motion-scene';out=r/'global';out.mkdir(exist_ok=True);cd=out/'frames';cd.mkdir(exist_ok=True)
cv2.setNumThreads(4);est=cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM);font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',20);frames=sorted((s/'linear').glob('*.rgba'));YY,XX=np.mgrid[:512,:512];P=np.stack([(XX+.5)/512,(YY+.5)/512,np.ones((512,512))],axis=-1);stats=[];last=None
for idx,i in enumerate(range(2,len(frames)-2)):
 d=out/f'{idx:03d}';d.mkdir(exist_ok=True)
 prev=np.array(Image.open(s/'frames'/('frame_%04d.png'%(i-1))).convert('RGB'));cur=np.array(Image.open(s/'frames'/('frame_%04d.png'%(i+1))).convert('RGB'))
 f=est.calc(cv2.cvtColor(cur,cv2.COLOR_RGB2GRAY),cv2.cvtColor(prev,cv2.COLOR_RGB2GRAY),None)
 y,x=np.mgrid[8:512:16,8:512:16];a=np.stack([x.ravel(),y.ravel()],axis=1).astype(np.float32);b=a+f[y,x].reshape(-1,2)
 cv2.setRNGSeed(314);H,mask=cv2.findHomography(a,b,cv2.RANSAC,2.0,maxIters=1500,confidence=.99)
 valid=H is not None and mask.mean()>.35
 if valid:
  S=np.diag([512.,512.,1.]);H=np.linalg.inv(S)@H@S;H/=H[2,2]
  corners=np.array([[0,0,1],[0,1,1],[1,0,1],[1,1,1]])@H.T
  valid=np.isfinite(H).all() and np.all(corners[:,2]>.5) and np.max(np.linalg.norm(corners[:,:2]/corners[:,2:]-np.array([[0,0],[0,1],[1,0],[1,1]]),axis=1))<.2
 if not valid:H=np.eye(3)
 # Modest temporal damping preserves a single projective mapping (no blending pixels).
 if last is not None:H=.7*H+.3*last
 last=H.copy();Q=P@H.T;q=Q[:,:,:2]/Q[:,:,2:];mapping=(P[:,:,:2]-q).astype(np.float32);path=d/'mapping.f32';np.stack([mapping,mapping]).tofile(path)
 env=os.environ.copy();env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation';env['NX_MOTION_FIELD_OVERRIDE']=str(path)
 p=subprocess.run([str(r/'build-dense/motion_gpu_truth'),'1','0','0',str(frames[i-2]),str(frames[i]),str(frames[i+2])],cwd=d,env=env,capture_output=True,text=True,check=True);(d/'run.log').write_text(p.stdout+p.stderr);m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',p.stdout);stats.append(dict(index=idx,held=float(m[1]),rmse=float(m[2]),valid=bool(valid),inliers=float(mask.mean()) if mask is not None else 0,H=H.tolist()))
 im=Image.new('RGB',(1536,552),'#101725');dr=ImageDraw.Draw(im)
 for k,(path,title) in enumerate([(r/'coherent'/f'{idx:03d}'/'warped.ppm','Shared regions — previous'),(d/'warped.ppm','Single-transform fallback'),(d/'truth.ppm','Correct future')]):im.paste(Image.open(path),(k*512,40));dr.text((k*512+8,8),title,font=font,fill='white')
 im.save(cd/f'{idx:03d}.png')
(out/'stats.json').write_text(json.dumps(stats,indent=2));print('mean rmse',np.mean([x['rmse'] for x in stats]),'valid',sum(x['valid'] for x in stats),flush=True)
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(cd/'%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,345)) for p in sorted(cd.glob('*.png'))[::2]];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
