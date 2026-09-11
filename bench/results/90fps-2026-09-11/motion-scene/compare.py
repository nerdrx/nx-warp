from pathlib import Path
import subprocess,os,json,re
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent
frames=sorted((r/'frames').glob('*.png'))
assert len(frames)>=12, len(frames)
linear=r/'linear';linear.mkdir(exist_ok=True)
lut=[round(255*(v/255/12.92 if v/255<=.04045 else ((v/255+.055)/1.055)**2.4)) for v in range(256)]
for p in frames:(linear/(p.stem+'.rgba')).write_bytes(Image.open(p).convert('RGB').resize((512,512)).point(lut*3).convert('RGBA').tobytes())
font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',20)
exe=r.parent/'motion-photo/build/motion_gpu_truth';env=os.environ.copy();env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
for gap in [1,2]:
 out=r/f'gap{gap}';out.mkdir(exist_ok=True);stats=[]
 for idx,i in enumerate(range(gap,len(frames)-gap)):
  sample=out/f'{idx:03d}';sample.mkdir(exist_ok=True)
  paths=[linear/(frames[j].stem+'.rgba') for j in [i-gap,i,i+gap]]
  p=subprocess.run([str(exe),'1','0','0',*map(str,paths)],cwd=sample,env=env,capture_output=True,text=True,check=True)
  (sample/'run.log').write_text(p.stdout+p.stderr)
  values=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',p.stdout);stats.append({'previous':i-gap,'current':i,'truth':i+gap,'held_rmse':float(values[1]),'warped_rmse':float(values[2])})
  canvas=Image.new('RGB',(1536,552),'#101725');draw=ImageDraw.Draw(canvas)
  for col,(kind,title) in enumerate([('held','Held old frame'),('warped','Actual NX GPU prediction'),('truth','Correct rendered future')]):
   im=Image.open(sample/(kind+'.ppm'));canvas.paste(im,(col*512,40));draw.text((col*512+8,8),title,font=font,fill='white')
  canvas.save(out/f'comparison-{idx:03d}.png')
 (out/'stats.json').write_text(json.dumps(stats,indent=2))
 subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'comparison-%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison-slow.mp4')],check=True)
 thumbs=[Image.open(p).resize((768,276)) for p in sorted(out.glob('comparison-*.png'))[::2]]
 thumbs[0].save(out/'preview.gif',save_all=True,append_images=thumbs[1:],duration=133,loop=0)
 print('gap',gap,'frames',len(stats),'mean held',sum(x['held_rmse'] for x in stats)/len(stats),'warp',sum(x['warped_rmse'] for x in stats)/len(stats),flush=True)
