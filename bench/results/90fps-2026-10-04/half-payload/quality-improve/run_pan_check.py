#!/usr/bin/env python3
"""Three-frame synthetic pan stability check for dark fixture; offline only."""
import hashlib,json,math,subprocess
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
ROOT=Path(__file__).resolve().parent
ENC=ROOT/'harness/build/astc-gpu'
DEC=Path('/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/decode_astc')
SRC=ROOT/'fixtures/dark-left.png'; OUT=ROOT/'motion/dark'; OUT.mkdir(parents=True,exist_ok=True)
img=np.asarray(Image.open(SRC).convert('RGB'),dtype=np.uint8); h,w,_=img.shape
qs={'current-q2':2,'q3-weight4-reference':3,'A-edge-gated':7,'B-luma-chroma':8}
frames={}
for shift in (0,1,2):
 frame=img[:,np.maximum(np.arange(w)-shift,0),:]
 rgba=np.concatenate([frame,np.full((h,w,1),255,dtype=np.uint8)],axis=2)
 raw=OUT/f'pan-{shift}.rgba';raw.write_bytes(rgba.tobytes())
 frames[shift]={'source':frame,'decoded':{}}
 for label,q in qs.items():
  astc=OUT/f'{label}-pan-{shift}.astc'
  subprocess.run([str(ENC),str(raw),str(astc),str(w),str(h),'3',str(q),'resident'],cwd=ENC.parent.parent,check=True,stdout=subprocess.DEVNULL)
  decoded=OUT/f'{label}-pan-{shift}.rgba'
  proc=subprocess.run([str(DEC),str(astc),str(decoded)],capture_output=True,text=True)
  if proc.returncode or not decoded.exists() or decoded.stat().st_size!=w*h*4: raise RuntimeError(f'decode invalid: {astc}: {proc.returncode} {proc.stderr}')
  rgbaout=np.fromfile(decoded,dtype=np.uint8).reshape(h,w,4)[:,:,:3].copy()
  payload=astc.read_bytes()[16:]; rawblocks=OUT/f'{label}-pan-{shift}.blocks';rawblocks.write_bytes(payload)
  subprocess.run(['zstd','-3','-q','-f',str(rawblocks),'-o',str(OUT/f'{label}-pan-{shift}.blocks.zst3')],check=True)
  d=frame.astype(np.int16)-rgbaout.astype(np.int16); mse=float(np.mean(d.astype(np.float64)**2)); mae=float(np.abs(d).mean())
  frames[shift]['decoded'][label]=rgbaout
  frames[shift].setdefault('metrics',{})[label]={'psnr_db':float('inf') if mse==0 else 10*math.log10(65025/mse),'mae':mae,'astc_sha256':hashlib.sha256(astc.read_bytes()).hexdigest(),'zstd3_bytes':(OUT/f'{label}-pan-{shift}.blocks.zst3').stat().st_size,'decoder_returncode':proc.returncode}
# Block-origin instability: align each later decoded pan frame back to frame 0,
# compare only common, non-clamped region to the shift-invariant decoded reference.
stability={}
for label in qs:
 ref=frames[0]['decoded'][label]
 stability[label]={}
 for shift in (1,2):
  now=frames[shift]['decoded'][label]
  a=now[:,shift:,:].astype(np.int16);b=ref[:,:w-shift,:].astype(np.int16)
  stability[label][str(shift)]={'aligned_mae':float(np.abs(a-b).mean()),'aligned_max_abs':int(np.abs(a-b).max())}
# Same crop, no resampling; rows identify reference/quality, columns pan offset.
box=(350,80,650,320); cw,ch=box[2]-box[0],box[3]-box[1]; label_h=28
sheet=Image.new('RGB',(3*cw,4*(ch+label_h)),'#111');draw=ImageDraw.Draw(sheet)
try:font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',16)
except OSError:font=ImageFont.load_default()
for col,shift in enumerate((0,1,2)):
 for row,name in enumerate(('source','current-q2','q3-weight4-reference','A-edge-gated','B-luma-chroma')):
  im=frames[shift]['source'] if name=='source' else frames[shift]['decoded'][name]
  tile=Image.fromarray(im[box[1]:box[3],box[0]:box[2]])
  x=col*cw;y=row*(ch+label_h)+label_h
  sheet.paste(tile,(x,y));draw.text((x+5,y-label_h+5),f'{name} · pan {shift}px',font=font,fill='white')
sheet.save(OUT/'pan-1to1-contact.png',optimize=True)
report={'scope':'Offline synthetic 0/1/2px horizontal pan of dark 2176x2176 fixture. Not live motion, network, or headset evidence.','source':str(SRC),'source_sha256':hashlib.sha256(SRC.read_bytes()).hexdigest(),'dims':[w,h],'pan_mapping':'frame[x] = source[max(x-shift,0)] (edge clamp, no wrap)','quality_variants':qs,'measurements':{str(k):v['metrics'] for k,v in frames.items()},'aligned_pan_instability':stability,'contact_sheet':'pan-1to1-contact.png'}
(OUT/'pan-metrics.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'measurements':report['measurements'],'aligned_pan_instability':stability},indent=2))
