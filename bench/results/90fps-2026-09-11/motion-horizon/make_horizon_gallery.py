from pathlib import Path
import json,subprocess,shutil
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;out=r/'horizon-gallery';(out/'frames').mkdir(parents=True,exist_ok=True)
f=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19)
a=json.loads((r/'tracked-horizon/scores.json').read_text());long={x['frame']:x for x in json.loads((r/'tracked-merged/scores.json').read_text())};rows=[]
for k,x in enumerate(a):
 idx=x['frame'];im=Image.new('RGB',(1536,1104),'#101725')
 im.paste(Image.open(r/'tracked-horizon/frames'/f'{k:03d}.png'),(0,0));im.paste(Image.open(r/'tracked-merged/frames'/f'{idx:03d}.png'),(0,552));d=ImageDraw.Draw(im);d.rectangle((0,552,1536,591),fill='#101725')
 for j,t in enumerate(['Held current','Predict +33.33ms (CPU)','Truth +33.33ms']):d.text((j*512+8,560),t,font=f,fill='white')
 im.save(out/'frames'/f'{k:03d}.png');rows.append(dict(source_frame=idx+3,target_short=idx+3+2/3,target_long=idx+5,short_rmse=x['rmse'],short_held=x['held_rmse'],long_rmse=long[idx]['rmse'],long_held=long[idx]['held_rmse']))
(out/'paired-scores.json').write_text(json.dumps(rows,indent=2))
subprocess.run(['ffmpeg','-v','error','-y','-framerate','3.75','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,690)) for p in sorted((out/'frames').glob('*.png'))];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=267,loop=0)
