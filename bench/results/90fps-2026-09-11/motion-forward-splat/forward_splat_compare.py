"""Offline source-anchored forward splat. No device or live configuration changes."""
from pathlib import Path
import sys,time,json,subprocess
r=Path(__file__).resolve().parent
sys.path.insert(0,str(r/'python-deps'))
import numpy as np
from PIL import Image,ImageDraw,ImageFont
out=r/'forward-splat';(out/'frames').mkdir(parents=True,exist_ok=True)
y,x=np.mgrid[:512,:512];p=np.stack([x,y],-1).astype(np.float32);rows=[]
def field_sample(f):
 g=np.clip((p+.5)/8-.5,0,63);c=np.floor(g).astype(int);n=np.minimum(c+1,63);w=g-c
 return (f[c[:,:,1],c[:,:,0]]*(1-w[:,:,0,None])*(1-w[:,:,1,None])+f[c[:,:,1],n[:,:,0]]*w[:,:,0,None]*(1-w[:,:,1,None])+f[n[:,:,1],c[:,:,0]]*(1-w[:,:,0,None])*w[:,:,1,None]+f[n[:,:,1],n[:,:,0]]*w[:,:,0,None]*w[:,:,1,None])*512
def linear(a):
 a=a/255.;return np.where(a<=.04045,a/12.92,((a+.055)/1.055)**2.4)
def srgb(a):return np.uint8(np.clip(np.where(a<=.0031308,a*12.92,1.055*np.maximum(a,0)**(1/2.4)-.055)*255+.5,0,255))
def centre(a):
 a=a.astype(float);yy,xx=np.where((a[:,:,1]>1.4*a[:,:,0])&(a[:,:,1]>1.15*a[:,:,2])&(a[:,:,1]>35));return np.array([xx.mean(),yy.mean()]) if len(xx)>=100 else None
for i in range(32):
 d=r/'gpu-cap/full'/f'{i:03d}';held=np.array(Image.open(d/'held.ppm'));truth=np.array(Image.open(d/'truth.ppm'));full=np.array(Image.open(d/'warped.ppm'));cap=np.array(Image.open(r/'gpu-cap/cap'/f'{i:03d}'/'warped.ppm'))
 f=np.fromfile(d/'field.f32',np.float32).reshape(2,64,64,2)[0];q=p+field_sample(f);base=np.floor(q).astype(int);frac=q-base;src=linear(held).reshape(-1,3);acc=np.zeros((512*512,3));weight=np.zeros(512*512);start=time.perf_counter()
 for dx,dy in [(0,0),(1,0),(0,1),(1,1)]:
  xx=base[:,:,0]+dx;yy=base[:,:,1]+dy;w=(frac[:,:,0] if dx else 1-frac[:,:,0])*(frac[:,:,1] if dy else 1-frac[:,:,1]);valid=(xx>=0)&(xx<512)&(yy>=0)&(yy<512);ids=(yy[valid]*512+xx[valid]);wv=w[valid];sv=src[valid.ravel()]
  weight+=np.bincount(ids,weights=wv,minlength=512*512)
  for ch in range(3):acc[:,ch]+=np.bincount(ids,weights=wv*sv[:,ch],minlength=512*512)
 valid=weight>1e-6;result=linear(cap).reshape(-1,3);result[valid]=acc[valid]/weight[valid,None];pred=srgb(result.reshape(512,512,3));ms=(time.perf_counter()-start)*1000
 Image.fromarray(pred).save(out/f'{i:03d}.png');row={'frame':i,'cpu_splat_ms':ms,'uncovered_fraction':float(np.mean(~valid.reshape(512,512)[64:-64,64:-64])),'rmse':float(np.sqrt(np.mean((pred[64:-64,64:-64].astype(float)-truth[64:-64,64:-64])**2)))}
 c=[centre(a) for a in [held,pred,truth]]
 if all(v is not None for v in c) and np.linalg.norm(c[2]-c[0])>=4:
  delta=c[2]-c[0];row.update(error=float(np.linalg.norm(c[1]-c[2])),progress=float(np.dot(c[1]-c[0],delta)/np.dot(delta,delta)))
 rows.append(row)
 im=Image.new('RGB',(2048,552),'#101018');dr=ImageDraw.Draw(im);font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',20)
 for j,(a,title) in enumerate([(cap,'Capped 11.11 ms'),(full,'Original full 33.33 ms'),(pred,'Forward splat 33.33 ms (CPU)'),(truth,'Correct future')]):im.paste(Image.fromarray(a),(j*512,40));dr.text((j*512+8,9),title,font=font,fill='white')
 im.save(out/'frames'/f'{i:03d}.png')
summary={k:float(np.mean([a[k] for a in rows if k in a])) for k in ['rmse','error','uncovered_fraction','cpu_splat_ms']};summary['median_progress']=float(np.median([a['progress'] for a in rows if 'progress' in a]));summary['eligible']=sum('progress' in a for a in rows)
(out/'scores.json').write_text(json.dumps({'summary':summary,'frames':rows},indent=2));print(summary,flush=True)
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','18','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(out/'frames'/f'{i:03d}.png').resize((1280,345)) for i in range(0,32,2)];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
