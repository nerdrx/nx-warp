from pathlib import Path
import subprocess,os,json,re
import numpy as np
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent/'robust';r.mkdir(exist_ok=True);s=r.parent.parent/'motion-scene';frames=sorted((s/'linear').glob('*.rgba'));env=os.environ.copy();env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation';exe=s/'build8/motion_gpu_truth'
def fit_regions(field,colour):
 # Connected image-colour/motion regions, no scene IDs or future pixels.
 h,w,_=field.shape;seen=np.zeros((h,w),bool);result=field.copy();groups=accepted=cells=0
 for y in range(h):
  for x in range(w):
   if seen[y,x]:continue
   seen[y,x]=1;q=[(y,x)]
   for yy,xx in q:
    for ny,nx in [(yy-1,xx),(yy+1,xx),(yy,xx-1),(yy,xx+1)]:
     if 0<=ny<h and 0<=nx<w and not seen[ny,nx] and np.linalg.norm(colour[ny,nx]-colour[yy,xx])<38:
      seen[ny,nx]=1;q.append((ny,nx))
   if len(q)<12:continue
   groups+=1;ys,xs=np.array(q).T;X=np.stack([(xs+.5)/w,(ys+.5)/h,np.ones(len(q))],axis=1);Y=field[ys,xs];mask=np.ones(len(q),bool)
   # Reject contaminated vector samples using deterministic consensus fitting.
   rng=np.random.default_rng(1234+groups);best=None;best_count=0;best_error=float('inf')
   for trial in range(64):
    ids=rng.choice(len(q),3,replace=False)
    if abs(np.linalg.det(X[ids]))<1e-5:continue
    candidate=np.linalg.solve(X[ids],Y[ids]);res=np.linalg.norm(X@candidate-Y,axis=1)*512
    support=res<2.5;count=int(support.sum());error=float(np.minimum(res,2.5).sum())
    if count>best_count or (count==best_count and error<best_error):best=support;best_count=count;best_error=error
   if best is None or best_count<max(8,len(q)*.35):continue
   coef=np.linalg.lstsq(X[best],Y[best],rcond=None)[0];pred=X@coef
   if np.linalg.norm(coef[:2])>2.:continue
   err=np.linalg.norm(pred-Y,axis=1)*512
   # Replace only inliers; leave unsupported pixels on original dense flow.
   good=np.ones(len(q),bool);result[ys[good],xs[good]]=pred[good];accepted+=1;cells+=int(good.sum())
 return result,dict(groups=groups,accepted=accepted,cells=cells)
font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',20);out=r/'frames';out.mkdir(exist_ok=True);stats=[]
for idx,i in enumerate(range(2,len(frames)-2)):
 d=r/f'{idx:03d}';d.mkdir(exist_ok=True);args=[str(exe),'1','0','0',str(frames[i-2]),str(frames[i]),str(frames[i+2])]
 p=subprocess.run(args,cwd=d,env=env,capture_output=True,text=True,check=True);(d/'baseline.log').write_text(p.stdout+p.stderr)
 field=np.fromfile(d/'field.f32',np.float32).reshape(2,64,64,2);rgb=np.array(Image.open(s/'frames'/('frame_%04d.png'%(i+1))).convert('RGB'),float);colour=rgb.reshape(64,8,64,8,3).mean(axis=(1,3));new,info=fit_regions(field[0],colour);field[:]=new;path=d/'grouped.f32';field.astype(np.float32).tofile(path)
 e=env.copy();e['NX_MOTION_FIELD_OVERRIDE']=str(path);p=subprocess.run(args,cwd=d,env=e,capture_output=True,text=True,check=True);(d/'grouped.log').write_text(p.stdout+p.stderr);m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',p.stdout);stats.append(dict(index=idx,held=float(m[1]),grouped=float(m[2]),**info))
 im=Image.new('RGB',(1536,552),'#101725');draw=ImageDraw.Draw(im)
 for k,(src,kind,title) in enumerate([(s/'grid8'/f'{idx:03d}','warped','Original 8px grid'),(d,'warped','Robust shared transforms'),(d,'truth','Correct rendered future')]):
  im.paste(Image.open(src/(kind+'.ppm')),(k*512,40));draw.text((k*512+8,8),title,font=font,fill='white')
 im.save(out/f'{idx:03d}.png')
(r/'stats.json').write_text(json.dumps(stats,indent=2));print('mean',sum(x['grouped'] for x in stats)/len(stats),'cells',sum(x['cells'] for x in stats)/len(stats),flush=True)
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(r/'regions-slow.mp4')],check=True)
thumbs=[Image.open(p).resize((960,345)) for p in sorted(out.glob('*.png'))[::2]];thumbs[0].save(r/'regions.gif',save_all=True,append_images=thumbs[1:],duration=133,loop=0)
