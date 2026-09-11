"""Evaluation-only green-block silhouette alignment; no masks feed prediction."""
from pathlib import Path
import json,subprocess
import numpy as np
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent;out=r/'alignment';(out/'frames').mkdir(parents=True,exist_ok=True);font=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',18)
def measure(im):
 a=np.array(im,dtype=float);m=(a[:,:,1]>1.4*a[:,:,0])&(a[:,:,1]>1.15*a[:,:,2])&(a[:,:,1]>35)
 y,x=np.where(m)
 if len(x)<100:return None,m
 return np.array([x.mean(),y.mean()]),m
rows=[]
for i in range(32):
 d=r/'gpu-cap/full'/f'{i:03d}';ims={'held':Image.open(d/'held.ppm').convert('RGB'),'full':Image.open(d/'warped.ppm').convert('RGB'),'cap':Image.open(r/'gpu-cap/cap'/f'{i:03d}'/'warped.ppm').convert('RGB'),'truth':Image.open(d/'truth.ppm').convert('RGB')}
 measurements={k:measure(v) for k,v in ims.items()};cent={k:v[0] for k,v in measurements.items()};row={'frame':i,'areas':{k:int(v[1].sum()) for k,v in measurements.items()},'centroids':{k:v.tolist() if v is not None else None for k,v in cent.items()}}
 valid=all(v is not None for v in cent.values());delta=cent['truth']-cent['held'] if valid else np.zeros(2);dist=float(np.linalg.norm(delta));row['held_target_distance_px']=dist;row['eligible']=bool(valid and dist>=4)
 if valid:
  for k in ['held','full','cap']:row[k+'_error_px']=float(np.linalg.norm(cent[k]-cent['truth']))
 if row['eligible']:
  for k in ['full','cap']:row[k+'_progress']=float(np.dot(cent[k]-cent['held'],delta)/np.dot(delta,delta))
 rows.append(row);canvas=Image.new('RGB',(1536,570),'#101725');dr=ImageDraw.Draw(canvas)
 for j,k in enumerate(['held','full','cap']):
  im=ims[k].copy();draw=ImageDraw.Draw(im)
  if valid:
   x,y=cent[k];tx,ty=cent['truth'];draw.ellipse((x-5,y-5,x+5,y+5),outline='white',width=2);draw.line((tx-8,ty,tx+8,ty),fill='#ff55ff',width=2);draw.line((tx,ty-8,tx,ty+8),fill='#ff55ff',width=2)
  canvas.paste(im,(j*512,55));dr.text((j*512+8,7),{'held':'Held current','full':'Full GPU prediction','cap':'Capped GPU prediction'}[k],font=font,fill='white');dr.text((j*512+8,29),'White: measured centre | Pink: target',font=font,fill='white')
 canvas.save(out/'frames'/f'{i:03d}.png')
(out/'scores.json').write_text(json.dumps(rows,indent=2));eligible=[x for x in rows if x['eligible']]
summary={'eligible_frames':len(eligible),'minimum_displacement_px':4,'mean_centroid_error_px':{k:float(np.mean([x[k+'_error_px'] for x in eligible])) for k in ['held','full','cap']},'median_projected_progress':{k:float(np.median([x[k+'_progress'] for x in eligible])) for k in ['full','cap']}}
(out/'summary.json').write_text(json.dumps(summary,indent=2));print(summary)
subprocess.run(['ffmpeg','-v','error','-y','-framerate','15','-i',str(out/'frames/%03d.png'),'-c:v','libx264','-crf','17','-pix_fmt','yuv420p','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
ims=[Image.open(p).resize((960,356)) for p in sorted((out/'frames').glob('*.png'))[::2]];ims[0].save(out/'comparison.gif',save_all=True,append_images=ims[1:],duration=133,loop=0)
