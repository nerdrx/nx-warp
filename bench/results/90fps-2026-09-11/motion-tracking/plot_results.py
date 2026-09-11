from pathlib import Path
import json
from PIL import Image,ImageDraw,ImageFont
r=Path(__file__).resolve().parent
names=[('tracked','Fine regions'),('tracked-coarse','Broad regions'),('tracked-history','Cache: 12 corners'),('tracked-history-v2','Cache: 4 corners'),('tracked-merged','Adjacent merge')]
summary={}
for key,label in names:
 a=json.loads((r/key/'scores.json').read_text());assert len(a)==32
 summary[key]={k:sum(v[k] for v in a)/len(a) for k in ['rmse','held_rmse','cpu_ms','moving']}
 summary[key]['better_frames']=sum(v['rmse']<v['held_rmse'] for v in a)
(r/'summary.json').write_text(json.dumps(summary,indent=2))
im=Image.new('RGB',(1100,600),'#111827');d=ImageDraw.Draw(im);f=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',19);big=ImageFont.truetype('/usr/share/fonts/TTF/DejaVuSans.ttf',25)
d.text((30,20),'Persistent-region diagnostics | 32-frame stress scene',font=big,fill='white')
d.text((260,80),'RGB RMSE (lower is better)',font=f,fill='#93c5fd');d.text((690,80),'Host CPU ms / image',font=f,fill='#fbbf24')
for i,(key,label) in enumerate(names):
 y=130+70*i;a=summary[key];d.text((25,y+5),label,font=f,fill='white')
 d.rectangle((260,y,260+a['rmse']*8,y+30),fill='#60a5fa');d.text((590,y+3),f"{a['rmse']:.3f}",font=f,fill='white')
 d.rectangle((690,y,690+a['cpu_ms']*3,y+30),fill='#fbbf24');d.text((990,y+3),f"{a['cpu_ms']:.1f}",font=f,fill='white')
# Explicit baseline, no implication that this measures live latency.
x=260+39.57059412386652*8;d.line((x,116,x,457),fill='white',width=2)
d.text((30,500),'White line: held-current RMSE 39.571. All timings are unisolated CPU diagnostics.',font=f,fill='#d1d5db')
d.text((30,535),'No live integration, headset performance or motion-to-photon latency proved.',font=f,fill='#d1d5db')
im.save(r/'results.png')
