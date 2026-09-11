"""Rotation-only camera alignment before the unchanged GPU estimator. No depth/IDs."""
from pathlib import Path
import sys,json,subprocess,os
import numpy as np
from PIL import Image,ImageDraw
r=Path(__file__).resolve().parent;sys.path.insert(0,str(r/'python-deps'));import cv2
out=r/'rotation-control';exe=r.parent/'motion-scene/build8/motion_gpu_truth'
def linear(im):
 a=np.asarray(im.convert('RGB'),np.float32)/255
 return np.where(a<=.04045,a/12.92,((a+.055)/1.055)**2.4)
def save(a,p):
 rgba=np.concatenate([np.clip(a*255+.5,0,255).astype('uint8'),np.full((*a.shape[:2],1),255,'uint8')],axis=2);rgba.tofile(p)
def align(prev,pp,cp):
 # Blender camera rays look down -Z; image Y points down.
 P=np.array(pp['projection']);C=np.array(cp['projection']);R=np.array(pp['rotation']).T@np.array(cp['rotation'])
 y,x=np.mgrid[:512,:512];rays=np.stack([(2*(x+.5)/512-1)/C[0,0],(1-2*(y+.5)/512)/C[1,1],-np.ones_like(x)],-1)
 v=rays@R.T;u=(P[0,0]*v[...,0]/(-v[...,2])+1)*256-.5;w=(1-P[1,1]*v[...,1]/(-v[...,2]))*256-.5
 valid=(u>=1)&(u<510)&(w>=1)&(w<510)&(v[...,2]<0)
 return cv2.remap(prev,u.astype('float32'),w.astype('float32'),cv2.INTER_LINEAR,borderMode=cv2.BORDER_REPLICATE),valid
poses=json.loads((out/'poses.json').read_text());moving=json.loads((out/'moving-poses.json').read_text());rows=[]
for kind,indices in [('rotation',range(1,8)),('mixed',range(3,34,4))]:
 frames=out/kind/'frames';frames.mkdir(parents=True,exist_ok=True)
 for n,f in enumerate(indices):
  if kind=='rotation':
   pp,cp=poses[str(f-1)],poses[str(f)];pim=Image.open(out/f'{f-1:03d}.png');cim=Image.open(out/f'{f:03d}.png')
  else:
   pp,cp=moving[str(f-2)],moving[str(f)];pim=Image.open(r.parent/f'motion-scene/frames/frame_{f-2:04d}.png');cim=Image.open(r.parent/f'motion-scene/frames/frame_{f:04d}.png')
  prev,cur=linear(pim),linear(cim);aligned,valid=align(prev,pp,cp);d=out/kind/f'{n:03d}';d.mkdir(exist_ok=True)
  save(prev,d/'previous.rgba');save(aligned,d/'aligned.rgba');save(cur,d/'current.rgba')
  row={'kind':kind,'frame':f,'valid_fraction':float(valid.mean()),'raw_linear_rmse':float(np.sqrt(np.mean((prev[valid]-cur[valid])**2))*255),'aligned_linear_rmse':float(np.sqrt(np.mean((aligned[valid]-cur[valid])**2))*255)}
  for name in ['previous','aligned']:
   work=d/name;work.mkdir(exist_ok=True);env=os.environ.copy();env.pop('NX_MOTION_FIELD_OVERRIDE',None);env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
   p=subprocess.run([str(exe),'1','0','0',str(d/f'{name}.rgba'),str(d/'current.rgba'),str(d/'current.rgba')],cwd=work,env=env,capture_output=True,text=True,check=True)
   log=p.stdout+p.stderr;assert 'Validation Error' not in log;(work/'run.log').write_text('\n'.join(x.rstrip() for x in log.splitlines())+'\n')
   flow=np.fromfile(work/'field.f32',np.float32).reshape(2,64,64,2)[0]*512
   mag=np.linalg.norm(flow[8:56,8:56],axis=-1);row[name+'_flow_median_px']=float(np.median(mag));row[name+'_flow_p95_px']=float(np.percentile(mag,95))
  srgb=np.where(aligned<=.0031308,aligned*12.92,1.055*aligned**(1/2.4)-.055)
  im=Image.new('RGB',(1536,548),'#101725');draw=ImageDraw.Draw(im)
  for k,(pic,label) in enumerate([(pim,'Previous'),(Image.fromarray(np.uint8(np.clip(srgb*255+.5,0,255))),'Previous aligned using camera rotation'),(cim,'Current reference')]):im.paste(pic.convert('RGB'),(512*k,36));draw.text((512*k+8,10),label,fill='white')
  im.save(frames/f'{n:03d}.png');rows.append(row);print(row,flush=True)
 subprocess.run(['ffmpeg','-v','error','-y','-framerate','4','-i',str(frames/'%03d.png'),'-c:v','libx264','-crf','18','-pix_fmt','yuv420p',str(out/kind/'comparison.mp4')],check=True)
 ims=[Image.open(p).resize((960,343)) for p in sorted(frames.glob('*.png'))];ims[0].save(out/kind/'comparison.gif',save_all=True,append_images=ims[1:],duration=250,loop=0)
(out/'scores.json').write_text(json.dumps(rows,indent=2))
