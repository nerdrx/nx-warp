"""Causal motion-history diagnostic. Past/current inputs only; future for scoring."""
from pathlib import Path
import numpy as np,json,subprocess
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;out=r/'temporal-field';out.mkdir(exist_ok=True)
y,x=np.mgrid[:512,:512];p=np.stack([x,y],-1).astype(np.float32)
def sample(a,q):
 h,w=a.shape[:2];q=np.clip(q,[0,0],[w-1,h-1]);b=np.floor(q).astype(int);n=np.minimum(b+1,[w-1,h-1]);f=q-b
 return (a[b[:,:,1],b[:,:,0]]*(1-f[:,:,0,None])*(1-f[:,:,1,None])+a[b[:,:,1],n[:,:,0]]*f[:,:,0,None]*(1-f[:,:,1,None])+a[n[:,:,1],b[:,:,0]]*(1-f[:,:,0,None])*f[:,:,1,None]+a[n[:,:,1],n[:,:,0]]*f[:,:,0,None]*f[:,:,1,None]).astype(np.float32)
def linear(a):
 a=a/255.;return np.where(a<=.04045,a/12.92,((a+.055)/1.055)**2.4)
def srgb(a):return np.uint8(np.clip(np.where(a<=.0031308,a*12.92,1.055*np.maximum(a,0)**(1/2.4)-.055)*255+.5,0,255))
def centre(a):
 a=a.astype(float);yy,xx=np.where((a[:,:,1]>1.4*a[:,:,0])&(a[:,:,1]>1.15*a[:,:,2])&(a[:,:,1]>35));return np.array([xx.mean(),yy.mean()]) if len(xx)>=100 else np.array([np.nan,np.nan])
methods=['cap','full','ema','transport','balanced','medium'];rows={k:[] for k in methods};centres={k:[] for k in methods+['truth','held']};prev_ema=None;prev_trans=None;prev_image=None
for i in range(32):
 d=r/'gpu-cap/full'/f'{i:03d}';held=np.array(Image.open(d/'held.ppm'));truth=np.array(Image.open(d/'truth.ppm'));img=linear(held).astype(np.float32);f=np.fromfile(d/'field.f32',np.float32).reshape(2,64,64,2)[0];flow=sample(f,(p+.5)/8-.5)*512
 if prev_ema is None:ema=flow;trans=flow;gate=np.zeros((512,512),bool)
 else:
  ema=.5*flow+.5*prev_ema;q=p-.5*flow
  hist=sample(prev_trans,q);colour_error=np.mean(np.abs(img-sample(prev_image,q)),axis=2)
  gate=(colour_error<.08)&np.all((q>=0)&(q<=511),axis=2)
  trans=np.where(gate[:,:,None],.5*flow+.5*hist,flow)
 prev_ema=ema;prev_trans=trans;prev_image=img;images={}
 for name,vec in [('cap',flow/3),('full',flow),('ema',ema),('transport',trans),('balanced',ema*2/3),('medium',flow*2/3)]:
  pred=srgb(sample(img,p-vec));images[name]=pred;dd=out/name;dd.mkdir(exist_ok=True);Image.fromarray(pred).save(dd/f'{i:03d}.png');centres[name].append(centre(pred));rows[name].append({'frame':i,'rmse':float(np.sqrt(np.mean((pred[64:-64,64:-64].astype(float)-truth[64:-64,64:-64])**2))),'history_fraction':float(np.mean(gate[64:-64,64:-64]))})
 centres['truth'].append(centre(truth));centres['held'].append(centre(held))
 im=Image.new('RGB',(2048,552),'#101018');dr=ImageDraw.Draw(im);font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',20)
 for j,(a,title) in enumerate([(images['cap'],'Cap 11.11 ms'),(images['full'],'Full 33.33 ms'),(images['balanced'],'22.22 ms + motion history'),(truth,'Correct future')]):im.paste(Image.fromarray(a),(512*j,40));dr.text((512*j+8,9),title,font=font,fill='white')
 (out/'frames').mkdir(exist_ok=True);im.save(out/'frames'/f'{i:03d}.png')
centres={k:np.array(v) for k,v in centres.items()};summary={};delta=centres['truth']-centres['held'];eligible=np.linalg.norm(delta,axis=1)>=4
for k in methods:
 residual=centres[k]-centres['truth'];second=np.diff(residual,n=2,axis=0);prog=np.sum((centres[k]-centres['held'])*delta,axis=1)/np.sum(delta*delta,axis=1)
 summary[k]={'mean_rmse':float(np.mean([a['rmse'] for a in rows[k]])),'mean_centre_error_px':float(np.nanmean(np.linalg.norm(residual[eligible],axis=1))),'median_progress':float(np.nanmedian(prog[eligible])),'rms_residual_second_difference_px':float(np.sqrt(np.nanmean(np.sum(second**2,axis=1)))),'eligible':int(np.sum(eligible))}
(out/'scores.json').write_text(json.dumps({'summary':summary,'frames':rows,'centres':{k:v.tolist() for k,v in centres.items()}},indent=2));print(json.dumps(summary,indent=2),flush=True)
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','18','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(out/'frames'/f'{i:03d}.png').resize((1280,345)) for i in range(0,32,2)];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
