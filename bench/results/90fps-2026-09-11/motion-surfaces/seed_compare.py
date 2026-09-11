from pathlib import Path
from seed_models import match_regions
import numpy as np,subprocess,os,json,re
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;s=r.parent/'motion-scene';out=r/'seed-moments';out.mkdir(exist_ok=True);canvasdir=out/'frames';canvasdir.mkdir(exist_ok=True)
frames=sorted((s/'linear').glob('*.rgba'));exe=r/'build-dense/motion_gpu_truth';font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',20)
YY,XX=np.mgrid[:512,:512];P=np.stack([(XX+.5)/512,(YY+.5)/512,np.ones((512,512))],axis=-1)
def chroma(rgb):
 return 128*rgb/np.maximum(rgb.sum(axis=2,keepdims=True),1)+.25*rgb
def fit_similarity(X,Y):
 n=len(X);D=np.zeros((n*2,4));D[0::2,0]=X[:,0];D[0::2,1]=-X[:,1];D[0::2,2]=1;D[1::2,0]=X[:,1];D[1::2,1]=X[:,0];D[1::2,3]=1
 a,b,tx,ty=np.linalg.lstsq(D,Y.reshape(-1),rcond=None)[0]
 return np.array([[a,b],[-b,a],[tx,ty]])
def regions(field,colour):
 labels=np.full((64,64),-1,int);models=[]
 for y in range(64):
  for x in range(64):
   if labels[y,x]>=0:continue
   ident=len(models);q=[(y,x)];labels[y,x]=ident
   for yy,xx in q:
    for ny,nx in [(yy-1,xx),(yy+1,xx),(yy,xx-1),(yy,xx+1)]:
     if 0<=ny<64 and 0<=nx<64 and labels[ny,nx]<0 and np.linalg.norm(colour[ny,nx]-colour[yy,xx])<38 and np.linalg.norm(colour[ny,nx]-colour[y,x])<38:labels[ny,nx]=ident;q.append((ny,nx))
   coef=np.zeros((3,2));valid=False
   if len(q)>=12:
    ys,xs=np.array(q).T;X=np.stack([(xs+.5)/64,(ys+.5)/64,np.ones(len(q))],axis=1);Y=field[ys,xs];mask=np.ones(len(q),bool)
    for _ in range(4):
     if mask.sum()<8:break
     coef=fit_similarity(X[mask],Y[mask]);pred=X@coef;err=np.linalg.norm(pred-Y,axis=1)*512;mask=err<max(2.,np.median(err)*1.7)
    valid=mask.sum()>=max(8,len(q)*.35) and np.linalg.norm(coef[:2])<=2 and 0.8 < np.linalg.det(np.eye(2)-coef[:2].T) < 1.25
   models.append((len(q),coef,valid))
 return labels,models
stats=[]
for idx,i in enumerate(range(2,len(frames)-2)):
 d=out/f'{idx:03d}';d.mkdir(exist_ok=True)
 field=np.fromfile(r/'coherent'/f'{idx:03d}'/'field.f32',np.float32).reshape(2,64,64,2)[0]
 rgb=np.array(Image.open(s/'frames'/('frame_%04d.png'%(i+1))).convert('RGB'),float);rgb=chroma(rgb);colour=rgb.reshape(64,8,64,8,3).mean(axis=(1,3));labels,models=regions(field,colour)
 previous_rgb=np.array(Image.open(s/'frames'/('frame_%04d.png'%(i-1))).convert('RGB'),float)
 coeff,valid,report=match_regions(chroma(previous_rgb),rgb,labels)
 models=[(m[0],coeff[k],bool(valid[k])) for k,m in enumerate(models)]
 (d/'matches.json').write_text(json.dumps(report,indent=2))

 # Snap each pixel to the closest-colour nearby cell, refining source boundaries.
 pixlabels=np.zeros((512,512),int);best=np.full((512,512),np.inf)
 for dy in [-1,0,1]:
  for dx in [-1,0,1]:
   cy=np.clip(YY//8+dy,0,63);cx=np.clip(XX//8+dx,0,63);err=((rgb-colour[cy,cx])**2).sum(axis=2);take=err<best;best[take]=err[take];pixlabels[take]=labels[cy,cx][take]
 # Identity fallback for exposed/unclaimed pixels. Large regions first; smaller
 # regions win overlaps, an image-only layering heuristic, NOT known depth.
 mapping=np.zeros((512,512,2),np.float32);covered=np.zeros((512,512),bool)
 for ident in sorted(range(len(models)),key=lambda n:models[n][0],reverse=True):
  count,coef,valid=models[ident]
  if not valid:continue
  delta=P@coef;q=P[:,:,:2]-delta;qx=np.floor(q[:,:,0]*512).astype(int);qy=np.floor(q[:,:,1]*512).astype(int)
  inside=(qx>=0)&(qx<512)&(qy>=0)&(qy<512);mask=inside&(pixlabels[np.clip(qy,0,511),np.clip(qx,0,511)]==ident)
  mapping[mask]=delta[mask];covered|=mask
 path=d/'mapping.f32';np.stack([mapping,mapping]).astype(np.float32).tofile(path)
 env=os.environ.copy();env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation';env['NX_MOTION_FIELD_OVERRIDE']=str(path)
 p=subprocess.run([str(exe),'1','0','0',str(frames[i-2]),str(frames[i]),str(frames[i+2])],cwd=d,env=env,capture_output=True,text=True,check=True);(d/'run.log').write_text(p.stdout+p.stderr);m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',p.stdout);stats.append(dict(index=idx,rmse=float(m[2]),covered=float(covered.mean()),regions=int(sum(x[2] for x in models))))
 im=Image.new('RGB',(1536,552),'#101725');dr=ImageDraw.Draw(im)
 for k,(path,title) in enumerate([(r/'coherent'/f'{idx:03d}'/'warped.ppm','Shared field — previous'),(d/'warped.ppm','Bounded-colour rigid tracking'),(d/'truth.ppm','Correct future')]):im.paste(Image.open(path),(k*512,40));dr.text((k*512+8,8),title,font=font,fill='white')
 im.save(canvasdir/f'{idx:03d}.png')
(out/'stats.json').write_text(json.dumps(stats,indent=2));print('mean rmse',np.mean([x['rmse'] for x in stats]),flush=True)
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(canvasdir/'%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,345)) for p in sorted(canvasdir.glob('*.png'))[::2]];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
