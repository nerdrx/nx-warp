from pathlib import Path
import subprocess,os,json,re
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;frames=sorted((r/'linear').glob('*.rgba'));font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',21)
env=os.environ.copy();env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
for block in [64]:
 out=r/'blur';out.mkdir(exist_ok=True);stats=[]
 for idx,i in enumerate(range(2,len(frames)-2)):
  d=out/f'{idx:03d}';d.mkdir(exist_ok=True)
  p=subprocess.run([str(r.parent/'motion-photo/build/motion_gpu_truth'),'1','0','0',str(frames[i-2]),str(frames[i]),str(frames[i+2])],cwd=d,env=env,capture_output=True,text=True,check=True)
  (d/'run.log').write_text(p.stdout+p.stderr);m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',p.stdout);stats.append({'current':i,'held_rmse':float(m[1]),'warped_rmse':float(m[2])})
 (out/'stats.json').write_text(json.dumps(stats,indent=2));print(block,sum(x['warped_rmse'] for x in stats)/len(stats),flush=True)

out=r/'blur-comparison';out.mkdir(exist_ok=True)
for idx in range(len(frames)-4):
 im=Image.new('RGB',(1536,552),'#101725');draw=ImageDraw.Draw(im)
 for k,(folder,kind,title) in enumerate([('gap2','warped','Original warp'),('blur','warped','Tiny motion blur (1.5px cap)'),('gap2','truth','Correct future')]):
  im.paste(Image.open(r/folder/f'{idx:03d}'/(kind+'.ppm')),(k*512,40));draw.text((k*512+8,8),title,font=font,fill='white')
 im.save(out/f'{idx:03d}.png')
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'blur-slow.mp4')],check=True)
thumbs=[Image.open(p).resize((960,345)) for p in sorted(out.glob('*.png'))[::2]]
thumbs[0].save(out/'blur.gif',save_all=True,append_images=thumbs[1:],duration=133,loop=0)
