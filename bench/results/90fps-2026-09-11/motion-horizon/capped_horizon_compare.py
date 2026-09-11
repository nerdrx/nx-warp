from pathlib import Path
import json,subprocess
import numpy as np
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;out=r/'horizon-cap';(out/'frames').mkdir(parents=True,exist_ok=True);rows=[];f=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19)
for k,idx in enumerate(range(0,32,4)):
 short=Image.open(r/'tracked-horizon/frames'/f'{k:03d}.png').crop((512,40,1024,552))
 long=Image.open(r/'tracked-merged/frames'/f'{idx:03d}.png');held=long.crop((0,40,512,552));full=long.crop((512,40,1024,552));truth=long.crop((1024,40,1536,552))
 gt=np.array(truth).astype(float);rm=lambda im:float(np.sqrt(np.mean((np.array(im).astype(float)[64:-64,64:-64]-gt[64:-64,64:-64])**2)))
 rows.append(dict(frame=idx,capped_rmse=rm(short),full_rmse=rm(full),held_rmse=rm(held)))
 im=Image.new('RGB',(1536,552),'#101725');d=ImageDraw.Draw(im)
 for j,(a,label) in enumerate([(full,'Full +33.33ms shift'),(short,'Shift capped at +11.11ms'),(truth,'Same truth +33.33ms')]):im.paste(a,(j*512,40));d.text((j*512+8,8),label,font=f,fill='white')
 im.save(out/'frames'/f'{k:03d}.png')
(out/'scores.json').write_text(json.dumps(rows,indent=2));print({key:sum(x[key] for x in rows)/8 for key in ['capped_rmse','full_rmse','held_rmse']})
subprocess.run(['ffmpeg','-v','error','-y','-framerate','3.75','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,345)) for p in sorted((out/'frames').glob('*.png'))];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=267,loop=0)
