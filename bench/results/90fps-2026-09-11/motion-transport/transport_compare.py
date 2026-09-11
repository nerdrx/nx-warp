"""CPU inverse-transport solve on actual GPU fields, then actual GPU image warp."""
from pathlib import Path
import sys
r=Path(__file__).resolve().parent;sys.path.insert(0,str(r/'python-deps'))
import cv2,numpy as np,subprocess,os,re,json
from PIL import Image,ImageDraw,ImageFont
cv2.setNumThreads(4);s=r.parent/'motion-scene';out=r/('transport-gated' if '--gate' in sys.argv else 'transport');(out/'frames').mkdir(parents=True,exist_ok=True);inputs=sorted((s/'linear').glob('*.rgba'));font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19);y,x=np.mgrid[:512,:512];p=np.stack([(x+.5)/512,(y+.5)/512],-1).astype(np.float32);rows=[]
def sample(field,q):
 g=np.clip(q*64-.5,0,63);c=np.floor(g).astype(np.int32);n=np.minimum(c+1,63);f=g-c;fx=f[:,:,0,None];fy=f[:,:,1,None]
 return ((field[c[:,:,1],c[:,:,0]]*(1-fx)+field[c[:,:,1],n[:,:,0]]*fx)*(1-fy)+(field[n[:,:,1],c[:,:,0]]*(1-fx)+field[n[:,:,1],n[:,:,0]]*fx)*fy).astype(np.float32)
for idx,i in enumerate(range(2,34)):
 d=out/f'{idx:03d}';d.mkdir(exist_ok=True);field=np.fromfile(r/'gpu-cap/full'/f'{idx:03d}'/'field.f32',np.float32).reshape(2,64,64,2);maps=[];errors=[]
 for v in range(2):
  q=p-sample(field[v],p)
  for n in range(6):q=.5*q+.5*(p-sample(field[v],q))
  residual=np.linalg.norm(q+sample(field[v],q)-p,axis=-1)*512
  if '--gate' in sys.argv:
   fallback=p-sample(field[v],p)/3
   q=np.where((residual>1)[:,:,None],fallback,q)
  maps.append(p-q);errors.append(float(np.mean(residual[64:-64,64:-64]>1)))
 path=d/'mapping.f32';np.stack(maps).astype(np.float32).tofile(path)
 env=os.environ.copy();env['NX_MOTION_FIELD_OVERRIDE']=str(path);env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
 run=subprocess.run([str(r/'build-dense/motion_gpu_truth'),'1','0','0',str(inputs[i-2]),str(inputs[i]),str(inputs[i+2])],cwd=d,env=env,capture_output=True,text=True,check=True);log=run.stdout+run.stderr;(d/'run.log').write_text(log);assert 'Validation Error' not in log
 m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',log);rows.append({'frame':idx,'held':float(m[1]),'rmse':float(m[2]),'residual_over_1px_fraction':float(np.mean(errors))})
 im=Image.new('RGB',(1536,552),'#101725');dr=ImageDraw.Draw(im)
 for j,(path,title) in enumerate([(r/'gpu-cap/full'/f'{idx:03d}'/'warped.ppm','Original GPU warp'),(d/'warped.ppm', 'Transport + residual fallback' if '--gate' in sys.argv else 'Transported field (CPU solve)'),(d/'truth.ppm','Same correct future')]):im.paste(Image.open(path),(j*512,40));dr.text((j*512+8,8),title,font=font,fill='white')
 im.save(out/'frames'/f'{idx:03d}.png');print(idx,rows[-1]['rmse'],flush=True)
(out/'scores.json').write_text(json.dumps(rows,indent=2));print('mean',np.mean([x['rmse'] for x in rows]),'nonconverged',np.mean([x['residual_over_1px_fraction'] for x in rows]),flush=True)
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,345)) for p in sorted((out/'frames').glob('*.png'))[::2]];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
