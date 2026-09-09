#!/usr/bin/env python3
"""CPU visual reference for the 2x2 and 3x3 low-poly Kuwahara probes."""
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).parent
STRENGTH = .85

def sample(a, ox, oy):
    H, W = a.shape[:2]
    y, x = np.indices(a.shape[:2], dtype=np.float32)
    x += ox; y += oy
    xf = np.floor(x).astype(int); yf = np.floor(y).astype(int)
    x0 = xf.clip(0, W-1); y0 = yf.clip(0, H-1)
    x1 = (xf + 1).clip(0, W-1); y1 = (yf + 1).clip(0, H-1)
    fx = (x - np.floor(x)).astype(np.float32)[...,None]; fy = (y - np.floor(y)).astype(np.float32)[...,None]
    return (a[y0,x0]*(1-fx)*(1-fy) + a[y0,x1]*fx*(1-fy) + a[y1,x0]*(1-fx)*fy + a[y1,x1]*fx*fy)

def low2(a):
    best = np.zeros_like(a); score = np.full((H,W), np.inf, np.float32); lum=lambda x: x@np.array([.299,.587,.114],np.float32)
    for q in range(4):
        sx = 1 if q&1 else -1; sy = 1 if q&2 else -1; vals=[]
        for j in range(2):
            for i in range(2): vals.append(sample(a, sx*(.5+2*i), sy*(.5+2*j)))
        z=np.stack(vals); m=z.mean(0); l=np.tensordot(z,np.array([.299,.587,.114],np.float32),axes=([3],[0])); v=(l*l).mean(0)-l.mean(0)**2; take=v<score; score[take]=v[take]; best[take]=m[take]
    return a*(1-STRENGTH)+best*STRENGTH

def low3(a):
    best=np.zeros_like(a); score=np.full((H,W),np.inf,np.float32); lum=lambda x: x@np.array([.299,.587,.114],np.float32)
    for q in range(4):
        sx=1 if q&1 else -1; sy=1 if q&2 else -1; aa=sample(a,sx*.5,sy*2.5); bb=sample(a,sx*2.5,sy*.5); v=(lum(aa)-lum(bb))**2; take=v<score; score[take]=v[take]; best[take]=((aa+bb)*.5)[take]
    return a*(1-STRENGTH)+best*STRENGTH

def low4(a):
    centre=sample(a,0,0); best=centre.copy(); score=np.full(centre.shape[:2],np.inf,np.float32); w=np.array([.299,.587,.114],np.float32)
    for q in range(4):
        sx=1 if q&1 else -1; sy=1 if q&2 else -1; tap=sample(a,sx*1.5,sy*1.5); v=np.tensordot(tap-centre,w,axes=([2],[0]))**2; take=v<score; score[take]=v[take]; best[take]=((centre+tap)*.5)[take]
    return centre*(1-STRENGTH)+best*STRENGTH

import argparse
ap=argparse.ArgumentParser(); ap.add_argument('--input',type=Path,required=True); args=ap.parse_args()
raw=np.asarray(Image.open(args.input).convert('RGB'),dtype=np.float32)/255
H,W=raw.shape[:2]
o2,o3,o4=low2(raw),low3(raw),low4(raw)
def im(x): return Image.fromarray((x.clip(0,1)*255+.5).astype(np.uint8))
font=ImageFont.load_default(); cell=544; out=Image.new('RGB',(3*cell,2*(cell+22)),'#111'); d=ImageDraw.Draw(out)
for i,(name,x) in enumerate([('source',raw),('sixteen-tap approximation',o2),('eight-tap approximation',o3),('four-tap approximation',o4),('four - sixteen',np.abs(o4-o2)*4),('four - eight',np.abs(o4-o3)*4)]):
    tile=im(x).resize((cell,cell)); xx=i%3*cell; yy=i//3*(cell+22); out.paste(tile,(xx,yy+22)); d.text((xx+5,yy+5),name,fill='white',font=font)
out.save(ROOT/'comparison.png',optimize=True)
def crisp(x):
    l=x@np.array([.299,.587,.114]); dx=np.diff(l,axis=1)[:-1,:]; dy=np.diff(l,axis=0)[:,:-1]; return float(np.hypot(dx,dy).mean())
metrics={'strength':STRENGTH,'mae_4_vs_2':float(np.abs(o4-o2).mean()),'mae_4_vs_3':float(np.abs(o4-o3).mean()),'p95_4_vs_2':float(np.percentile(np.abs(o4-o2),95)),'mean_luma_delta_4_vs_2':float((o4@np.array([.299,.587,.114])-o2@np.array([.299,.587,.114])).mean()),'border_crispness':{'sixteen':crisp(o2),'eight':crisp(o3),'four':crisp(o4)}}
(ROOT/'metrics.json').write_text(__import__('json').dumps(metrics,indent=2)+'\n')
