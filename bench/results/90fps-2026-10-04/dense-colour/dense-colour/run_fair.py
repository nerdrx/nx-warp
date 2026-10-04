import json, math, subprocess
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
ROOT=Path(__file__).resolve().parent; OUT=ROOT/'results'; ENC=ROOT/'harness/build/astc-gpu'
DEC=Path('/run/media/nerdrx/Lex/claude/nx-scratch/nx-xuastc-20261003/decode_astc')
records=[]
for scene,filename in [('crowd','crowd-500-reference.png'),('dark','dark-left.png'),('forest','forest-left.png')]:
    source_path=ROOT/'fixtures'/filename; source=Image.open(source_path).convert('RGB'); src=np.asarray(source,dtype=np.uint8); w,h=source.size
    raw=OUT/f'{scene}.rgba'; raw.write_bytes(source.convert('RGBA').tobytes())
    for bs in (8,6):
      for q in (6,2):
        stem=f'{scene}-{bs}x{bs}-q{q}-fair'; astc=OUT/f'{stem}.astc'
        subprocess.run([str(ENC),str(raw),str(astc),str(w),str(h),'3',str(q),str(bs),'resident'],cwd=ENC.parent.parent,check=True,stdout=subprocess.DEVNULL)
        blocks=astc.read_bytes()[16:]; comp=subprocess.run(['zstd','-q','-3','-c'],input=blocks,stdout=subprocess.PIPE,check=True).stdout
        zpath=OUT/f'{stem}.blocks.zst';zpath.write_bytes(comp)
        dec=OUT/f'{stem}.rgba'; subprocess.run([str(DEC),str(astc),str(dec)],check=True,stdout=subprocess.DEVNULL)
        arr=np.fromfile(dec,dtype=np.uint8).reshape(h,w,4)[:,:,:3]; diff=src.astype(np.int16)-arr.astype(np.int16); mse=float(np.square(diff.astype(np.float64)).mean())
        timing=json.loads(astc.with_suffix('.astc.json').read_text())
        records.append(dict(scene=scene,block=f'{bs}x{bs}',quality=q,psnr_db=100 if not mse else 10*math.log10(255**2/mse),mae=float(np.abs(diff).mean()),astc_block_bytes=len(blocks),zstd3_bytes=len(comp),gpu_encode_median_ms=timing['gpu_encode_median_ms'],gpu_encode_median_ticks=timing['gpu_encode_median_ticks'],gpu_encode_p95_ms=timing['gpu_encode_p95_ms'],timestamp_period_ns=timing['timestamp_period_ns'],timestamp_valid_bits=timing['timestamp_valid_bits'],device=timing['device'],decoded=True))
# Native 1:1 RGB crop: source, 8x8 and 6x6 q6.
scene='crowd'; source=Image.open(ROOT/'fixtures/crowd-500-reference.png').convert('RGB'); box=(350,80,850,480)
panel=Image.new('RGB',(1500,440),'#08060d');draw=ImageDraw.Draw(panel)
for x,label in ((0,'SOURCE'),(500,'8×8 · q6'),(1000,'6×6 · q6')):draw.text((x+8,8),label,fill='white')
panel.paste(source.crop(box),(0,40))
for idx,bs in enumerate((8,6),1):
 p=OUT/f'crowd-{bs}x{bs}-q6-fair.rgba'; a=np.fromfile(p,dtype=np.uint8).reshape(source.height,source.width,4);panel.paste(Image.fromarray(a[:,:,:3]).crop(box),(idx*500,40))
panel.save(OUT/'crowd-q6-fair-crop.png',optimize=True)
payload={'method':'Matching 5x5 inverse least-squares fits generated with the same Moore-Penrose pseudoinverse and bilinear texel-to-weight-grid mapping for 6x6 and 8x8; source image edge samples clamp; decoder-verified ASTC; zstd CLI -3 on ASTC block payload only. Timestamp query periods converted with valid timestamp bits; 3 warmups and 10 samples per output.','records':records}
(OUT/'fair-results.json').write_text(json.dumps(payload,indent=2)+'\n')
