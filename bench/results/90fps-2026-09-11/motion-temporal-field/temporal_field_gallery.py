from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
import subprocess
r=Path(__file__).resolve().parent;o=r/'temporal-field';(o/'overview').mkdir(exist_ok=True)
font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19)
for i in range(32):
 im=Image.new('RGB',(2048,1104),'#101018');d=ImageDraw.Draw(im)
 entries=[('cap','Cap 11.11 ms'),('medium','22.22 ms, no history'),('balanced','22.22 ms + history'),('truth','Correct future'),('full','Full 33.33 ms'),('ema','Full + same-pixel history'),('transport','Full + transported history'),('held','Held source')]
 for j,(k,label) in enumerate(entries):
  path=r/'gpu-cap/full'/f'{i:03d}'/(k+'.ppm') if k in ['held','truth'] else o/k/f'{i:03d}.png';x=j%4*512;y=j//4*552;im.paste(Image.open(path),(x,y+40));d.text((x+8,y+9),label,font=font,fill='white')
 im.save(o/'overview'/f'{i:03d}.png')
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(o/'overview/%03d.png'),'-c:v','libx264','-crf','18','-pix_fmt','yuv420p','-movflags','+faststart',str(o/'overview.mp4')],check=True)
