from pathlib import Path
import subprocess,json,shutil
from PIL import Image
r=Path(__file__).resolve().parent;live=r.parent/'motion-live';out=r.parent.parent/'nx-warp/bench/results/90fps-2026-09-11/pico-retained-repeat';out.mkdir(parents=True,exist_ok=True)
for n in ['0','1']:
 p=r/f'pico-retain-repeat{n}.json';shutil.copy2(p,out/p.name)
 for label in [f'pico-retain-repeat{n}',f'pico-retain-video{n}']:
  for p in live.glob(label+'*'):
   if p.suffix in ['.log','.json']:shutil.copy2(p,out/p.name)
 subprocess.run(['ffmpeg','-v','error','-y','-i',str(r/f'pico-retain-video{n}.mp4'),'-vf','scale=960:480','-c:v','libx264','-crf','20','-movflags','+faststart',str(out/f'pico-{n}.mp4')],check=True)
subprocess.run(['ffmpeg','-v','error','-y','-i',str(out/'pico-0.mp4'),'-i',str(out/'pico-1.mp4'),'-filter_complex',"[0:v]drawtext=fontfile=/usr/share/fonts/TTF/DejaVuSans.ttf:text='Three retained frames - separate capture':x=12:y=12:fontsize=20:fontcolor=white:box=1:boxcolor=black[a];[1:v]drawtext=fontfile=/usr/share/fonts/TTF/DejaVuSans.ttf:text='Four retained frames - separate capture':x=12:y=12:fontsize=20:fontcolor=white:box=1:boxcolor=black[b];[a][b]vstack=inputs=2[v]",'-map','[v]','-shortest','-c:v','libx264','-crf','20','-movflags','+faststart',str(out/'comparison.mp4')],check=True)
subprocess.run(['ffmpeg','-v','error','-y','-i',str(out/'comparison.mp4'),'-ss','2','-frames:v','1',str(out/'example.png')],check=True)
for f in ['repeat.py','analyze.py','package_repeat.py']:shutil.copy2(r/f,out/f)
print(out)
