from pathlib import Path
import subprocess,os,json,re
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;frames=sorted((r/'linear').glob('*.rgba'));font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',21)
env=os.environ.copy();env['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
for block in [32,16,8]:
 out=r/f'grid{block}';out.mkdir(exist_ok=True);stats=[]
 for idx,i in enumerate(range(2,len(frames)-2)):
  d=out/f'{idx:03d}';d.mkdir(exist_ok=True)
  p=subprocess.run([str(r/f'build{block}/motion_gpu_truth'),'1','0','0',str(frames[i-2]),str(frames[i]),str(frames[i+2])],cwd=d,env=env,capture_output=True,text=True,check=True)
  (d/'run.log').write_text(p.stdout+p.stderr);m=re.search(r'held_rmse=([\d.]+) warped_rmse=([\d.]+)',p.stdout);stats.append({'current':i,'held_rmse':float(m[1]),'warped_rmse':float(m[2])})
 (out/'stats.json').write_text(json.dumps(stats,indent=2));print(block,sum(x['warped_rmse'] for x in stats)/len(stats),flush=True)
out=r/'grid-comparison';out.mkdir(exist_ok=True)
for idx in range(len(frames)-4):
 im=Image.new('RGB',(1536,1104),'#101725');draw=ImageDraw.Draw(im)
 items=[('gap2','held','Held old frame'),('gap2','warped','64px grid — current'),('grid32','warped','32px grid'),('grid16','warped','16px grid'),('grid8','warped','8px grid'),('gap2','truth','Correct rendered future')]
 for k,(folder,kind,title) in enumerate(items):
  x=k%3*512;y=k//3*552;im.paste(Image.open(r/folder/f'{idx:03d}'/(kind+'.ppm')),(x,y+40));draw.text((x+8,y+8),title,font=font,fill='white')
 im.save(out/f'{idx:03d}.png')
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'all-grids-slow.mp4')],check=True)
thumbs=[Image.open(p).resize((960,690)) for p in sorted(out.glob('*.png'))[::2]]
thumbs[0].save(out/'all-grids.gif',save_all=True,append_images=thumbs[1:],duration=133,loop=0)
