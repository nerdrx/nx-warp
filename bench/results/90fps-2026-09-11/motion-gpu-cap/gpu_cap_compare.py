"""Actual GPU estimator/warp, equal future target, two extrapolation steps."""
from pathlib import Path
import subprocess,os,re,json,hashlib
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;s=r.parent/'motion-scene';out=r/'gpu-cap';(out/'frames').mkdir(parents=True,exist_ok=True)
exe=s/'build8/motion_gpu_truth';frames=sorted((s/'linear').glob('*.rgba'));font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19);rows=[]
for idx,i in enumerate(range(2,len(frames)-2)):
 row={'frame':idx,'source_frame':i+1,'future_frame':i+3}
 paths={}
 for name,step in [('full','1'),('cap','0.333333333')]:
  d=out/name/f'{idx:03d}';d.mkdir(parents=True,exist_ok=True);env=os.environ.copy();env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation';env.pop('NX_MOTION_FIELD_OVERRIDE',None)
  p=subprocess.run([str(exe),step,'0','0',str(frames[i-2]),str(frames[i]),str(frames[i+2])],cwd=d,env=env,capture_output=True,text=True,check=True)
  log=p.stdout+p.stderr;(d/'run.log').write_text(log);assert 'Validation Error' not in log
  m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',log);assert m
  row[name]=float(m[2]);row['held']=float(m[1]);paths[name]=d/'warped.ppm'
 # With identical input and estimator, held/target must be identical across runs.
 for name in ['held.ppm','truth.ppm']:assert (out/'full'/f'{idx:03d}'/name).read_bytes()==(out/'cap'/f'{idx:03d}'/name).read_bytes()
 rows.append(row);im=Image.new('RGB',(1536,552),'#101725');dr=ImageDraw.Draw(im)
 for k,(path,title) in enumerate([(paths['full'],'GPU: full +33.33ms shift'),(paths['cap'],'GPU: capped +11.11ms shift'),(d/'truth.ppm','Same truth +33.33ms')]):im.paste(Image.open(path),(k*512,40));dr.text((k*512+8,8),title,font=font,fill='white')
 im.save(out/'frames'/f'{idx:03d}.png');print(idx,row['full'],row['cap'],flush=True)
(out/'scores.json').write_text(json.dumps(rows,indent=2));(out/'binary.json').write_text(json.dumps({'path':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest()},indent=2))
print('means',{k:sum(x[k] for x in rows)/len(rows) for k in ['held','full','cap']},flush=True)
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,345)) for p in sorted((out/'frames').glob('*.png'))[::2]];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
