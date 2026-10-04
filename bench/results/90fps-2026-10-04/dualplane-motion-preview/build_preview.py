#!/usr/bin/env python3
"""Render only configured 512px-or-smaller crops from density-agent pan frames."""
from pathlib import Path
import hashlib, json, csv
from PIL import Image, ImageDraw, ImageFont

HERE=Path(__file__).resolve().parent
CFG=HERE/'inputs.json'
if not CFG.is_file():
    raise SystemExit('Waiting for density-agent final pan paths: create inputs.json from its provided read-only paths/hashes.')
cfg=json.loads(CFG.read_text())
assert len(cfg['cases']) == 4
records=[]
font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',16)

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def load_frame(path,spec):
    p=Path(path)
    if p.suffix.lower()=='.rgba':
        w,h=spec['width'],spec['height']
        raw=p.read_bytes(); assert len(raw)==w*h*4,(p,len(raw),w,h)
        return Image.frombytes('RGBA',(w,h),raw).convert('RGB')
    with Image.open(p) as im:
        assert im.size==(spec['width'],spec['height']),(p,im.size,spec)
        return im.convert('RGB')

def add_label(im,text):
    d=ImageDraw.Draw(im); d.rectangle((0,0,im.width,28),fill='white'); d.text((6,5),text,font=font,fill='#102033')
    return im

for case in cfg['cases']:
    name=case['name']; q=int(case['q']); crop=case['crop']
    x,y,w,h=map(int,(crop['x'],crop['y'],crop['width'],crop['height']))
    assert 1<=w<=512 and 1<=h<=512
    base=case['baseline_frames']; dual=case['dual_frames']
    assert len(base)==len(dual)==8,(name,len(base),len(dual))
    frames=[]
    for phase,(bf,df) in enumerate(zip(base,dual)):
        for role,item in [('baseline',bf),('dual',df)]:
            p=Path(item['path']); actual=sha(p); assert actual==item['sha256'],(p,actual,item['sha256'])
            records.append({'case':name,'q':q,'phase':phase,'role':role,'path':str(p),'sha256':actual,'size_bytes':p.stat().st_size})
        b=load_frame(bf['path'],cfg['dimensions']).crop((x,y,x+w,y+h))
        d=load_frame(df['path'],cfg['dimensions']).crop((x,y,x+w,y+h))
        panel=Image.new('RGB',(2*w,h+28),'white')
        panel.paste(add_label(b.copy(),'Baseline'),(0,28)); panel.paste(add_label(d.copy(),'Dual-plane candidate'),(w,28))
        draw=ImageDraw.Draw(panel); draw.text((6,5),f'{name} · q{q} · gate {case["selected_gate"]} · synthetic pan phase {phase+1}/8',font=font,fill='#102033')
        frames.append(panel)
    webp=HERE/f'{name}-q{q}-8phase.webp'
    frames[0].save(webp,save_all=True,append_images=frames[1:],duration=180,loop=0,lossless=True,method=4)
    sheet=Image.new('RGB',(4*2*w,2*(h+28)),'#e8edf3')
    for i,frame in enumerate(frames): sheet.paste(frame,((i%4)*2*w,(i//4)*(h+28)))
    still=HERE/f'{name}-q{q}-8phase.png'; sheet.save(still,optimize=True)
    records.extend([{'case':name,'q':q,'phase':'animated-preview','role':'webp','path':str(webp),'sha256':sha(webp),'size_bytes':webp.stat().st_size},
                    {'case':name,'q':q,'phase':'contact-sheet','role':'png','path':str(still),'sha256':sha(still),'size_bytes':still.stat().st_size}])
svg=HERE/'temporal-rms.svg'
records.append({'case':'all','q':'q2/q6','phase':'summary','role':'svg','path':str(svg),'sha256':sha(svg),'size_bytes':svg.stat().st_size})
with (HERE/'hashes.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=['case','q','phase','role','path','sha256','size_bytes'],lineterminator='\n');w.writeheader();w.writerows(records)
for item in cfg.get('source_files',{}).values():
    p=Path(item['path']); assert sha(p)==item['sha256'],p
manifest={'purpose':'Cropped visual preview of density-agent final exact-photo pan output; no full images copied.',
          'input_manifest':str(CFG),'source_photo_sha256':cfg.get('source_photo_sha256',{}),'source_files':cfg.get('source_files',{}),
          'synthetic_motion':'Eight phases from the agent-supplied one-pixel camera-pan fixture; synthetic pan is not VR/headset motion evidence.',
          'crop_limit_px':512,'dimensions':cfg['dimensions'],'cases':[{'name':c['name'],'q':c['q'],'selected_gate':c['selected_gate'],'source_roi':c['source_roi']} for c in cfg['cases']],'outputs':[r for r in records if r['role'] in ('webp','png','svg')],
          'input_frames':[r for r in records if r['role'] in ('baseline','dual')]}
(HERE/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'cases':len(cfg['cases']),'input_frames':len([r for r in records if r['role'] in ('baseline','dual')]),'outputs':len([r for r in records if r['role'] in ('webp','png','svg')])},indent=2))
