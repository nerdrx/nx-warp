from pathlib import Path
import re
from PIL import Image,ImageDraw
r=Path(__file__).resolve().parent;live=r.parent/'motion-live';im=Image.new('RGB',(1100,650),'#111827');d=ImageDraw.Draw(im)
for j,label in enumerate(['pico-jitter-candidates','pico-jitter-retain4']):
 rows=[]
 for line in (live/(label+'-client.log')).read_text().splitlines():
  m=re.search(r'warp timeline: display (\d+) frame (\d+) compositor (\d+) anchor (\d+) span (\d+) step ([\d.]+) field (true|false)',line)
  if m:rows.append((int(m[1]),int(m[4])+int(m[5])*float(m[6]) if m[7]=='true' else int(m[3])))
 rows=rows[300:421];top=50+j*310
 d.text((55,top-30),label+' | timestamp advance per refresh (not measured image motion)',fill='white')
 for v in [0,11.11,22.22,33.33]:
  y=top+230-v*6;d.line((55,y,1060,y),fill='#374151');d.text((5,y),f'{v:.1f}',fill='white')
 for k,(a,b) in enumerate(zip(rows,rows[1:])):
  v=(b[1]-a[1])/1e6;ex=(b[0]-a[0])/1e6;x=55+k*8
  d.ellipse((x-2,top+230-v*6-2,x+2,top+230-v*6+2),fill='#60a5fa');d.point((x,top+230-ex*6),fill='#fbbf24')
 d.text((55,top+247),'Blue: source + warp timeline step. Gold: requested display step. First 120 pairs after warm-up.',fill='white')
im.save(r/'retained-timeline.png')
