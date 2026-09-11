from pathlib import Path
import numpy as np,os,subprocess,json,re
from PIL import Image,ImageDraw
from coherent_field import clean
r=Path(__file__).resolve().parent;s=r.parent/'motion-scene';out=r/'coherent-field';out.mkdir(exist_ok=True);rows=[]
def read(p):return np.fromfile(p,np.uint8).reshape(512,512,4)[:,:,:3].astype(np.float32)/255
for kind,count in [('moving',32)]:
 frames=out/kind/'frames';frames.mkdir(parents=True,exist_ok=True)
 for idx in range(count):
  if kind=='static':
   base=r/'rotation-control/rotation'/f'{idx:03d}';prev=base/'aligned.rgba';cur=base/'current.rgba';future=cur;fieldpath=base/'aligned/field.f32';original=base/'aligned/warped.ppm'
  else:
   base=r/'gpu-cap/full'/f'{idx:03d}';prev=s/f'linear/frame_{idx+1:04d}.rgba';cur=s/f'linear/frame_{idx+3:04d}.rgba';future=s/f'linear/frame_{idx+5:04d}.rgba';fieldpath=base/'field.f32';original=base/'warped.ppm'
  field=np.fromfile(fieldpath,np.float32).reshape(2,64,64,2);filtered,keep=clean(read(cur),field[0]);d=out/kind/f'{idx:03d}';d.mkdir(exist_ok=True);mapping=d/'applied.f32';np.stack([filtered,filtered]).astype(np.float32).tofile(mapping)
  env=os.environ.copy();env['NX_MOTION_FIELD_OVERRIDE']=str(mapping);env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
  p=subprocess.run([str(s/'build8/motion_gpu_truth'),'1','0','0',str(prev),str(cur),str(future)],cwd=d,env=env,capture_output=True,text=True,check=True);log=p.stdout+p.stderr;assert 'Validation Error' not in log;(d/'run.log').write_text('\n'.join(a.rstrip() for a in log.splitlines())+'\n')
  # Old fixture exports estimated field over the override path; save applied mapping again.
  np.stack([filtered,filtered]).astype(np.float32).tofile(mapping)
  a=np.array(Image.open(original),np.float32)[64:-64,64:-64];b=np.array(Image.open(d/'truth.ppm'),np.float32)[64:-64,64:-64]
  match=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',log)
  rows.append({'kind':kind,'frame':idx,'original_rmse':float(np.sqrt(np.mean((a-b)**2))),'gated_rmse':float(match[2]),'held_rmse':float(match[1]),'smoothed_fraction':float(keep[8:56,8:56].mean()),'original_p95_px':float(np.percentile(np.linalg.norm(field[0,8:56,8:56]*512,axis=-1),95)),'gated_p95_px':float(np.percentile(np.linalg.norm(filtered[8:56,8:56]*512,axis=-1),95))})
  im=Image.new('RGB',(1536,548),'#101725');draw=ImageDraw.Draw(im)
  for k,(path,title) in enumerate([(original,'Original extrapolation'),(d/'warped.ppm','Local coherent motion'),(d/'truth.ppm','Static reference' if kind=='static' else 'Same future +33.33ms')]):im.paste(Image.open(path),(k*512,36));draw.text((k*512+8,10),title,fill='white')
  im.save(frames/f'{idx:03d}.png')
 subprocess.run(['ffmpeg','-v','error','-y','-framerate','15' if kind=='moving' else '4','-i',str(frames/'%03d.png'),'-c:v','libx264','-crf','18','-pix_fmt','yuv420p',str(out/kind/'comparison.mp4')],check=True)
 ims=[Image.open(p).resize((960,343)) for p in sorted(frames.glob('*.png'))][::2 if kind=='moving' else 1];ims[0].save(out/kind/'comparison.gif',save_all=True,append_images=ims[1:],duration=133 if kind=='moving' else 250,loop=0)
(out/'scores.json').write_text(json.dumps(rows,indent=2))
for kind in ['moving']:
 a=[v for v in rows if v['kind']==kind];print(kind,{k:float(np.mean([v[k] for v in a])) for k in a[0] if k not in ['kind','frame']})
