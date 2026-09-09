#!/usr/bin/env python3
"""Make crops and exact PLANAR centre policy diagram from decoded YUV420."""
from argparse import ArgumentParser
from pathlib import Path
import json
import numpy as np
from PIL import Image, ImageDraw, ImageFont

def read420(path, w, h, frame=0):
    n = w*h*3//2
    raw = np.fromfile(path, dtype=np.uint8, count=n, offset=frame*n)
    if raw.size != n: raise ValueError(f"short YUV frame in {path}")
    y = raw[:w*h].reshape(h,w)
    u = raw[w*h:w*h+w*h//4].reshape(h//2,w//2).repeat(2,0).repeat(2,1).astype(float)-128
    v = raw[w*h+w*h//4:].reshape(h//2,w//2).repeat(2,0).repeat(2,1).astype(float)-128
    yf=y.astype(float)
    rgb=np.stack((yf+1.402*v,yf-.344136*u-.714136*v,yf+1.772*u),-1).clip(0,255).astype('uint8')
    return rgb

def main():
    ap=ArgumentParser(); ap.add_argument('--root',type=Path,default=Path(__file__).parent)
    ap.add_argument('--width',type=int,default=4352); ap.add_argument('--height',type=int,default=2176)
    ap.add_argument('--frame',type=int,default=0); ap.add_argument('--crop',type=int,default=768); a=ap.parse_args()
    root,w,h=a.root,a.width,a.height; names=['source','control','colour','round-colour']; imgs=[]
    x0=w//4-a.crop//2; y0=h//2-a.crop//2
    for name in names: imgs.append(Image.fromarray(read420(root/f'{name}.yuv',w,h,a.frame)[y0:y0+a.crop,x0:x0+a.crop]))
    font=ImageFont.load_default(); cell=a.crop; out=Image.new('RGB',(2*cell,2*(cell+24)),'#101218'); d=ImageDraw.Draw(out)
    for i,(name,img) in enumerate(zip(names,imgs)):
        x,y=i%2*cell,i//2*(cell+24); out.paste(img,(x,y+24)); d.text((x+8,y+6),name,fill='white',font=font)
    out.save(root/'comparison-crop.png',optimize=True)
    # The encoder's jobs are per eye.  This test has two 2176-pixel eyes.
    eye_w=w//2; cols,rows=eye_w//64,h//64; ccols=min(cols,max(2,(cols//4)&~1)); crows=min(rows,max(2,(rows//4)&~1)); col0,row0=(cols-ccols)//2,(rows-crows)//2
    S=720; pol=Image.new('RGB',(S,S+70),'#101218'); pd=ImageDraw.Draw(pol); ox,oy,scale=40,30,min((S-80)/cols,(S-100)/rows)
    for r in range(rows):
        for c in range(cols):
            rx=(2*c+1-(2*col0+ccols))/ccols; ry=(2*r+1-(2*row0+crows))/crows; rad=(rx*rx+ry*ry)**.5; centre=rad<=1
            dist=np.ceil(max(0,rad*.886226925452758-1)*min(ccols,crows)*.5); colour='#8b5cf6' if centre else '#22c55e' if dist<=4 else '#334155'
            x,y=ox+c*scale,oy+r*scale; pd.rectangle((x,y,x+scale+.3,y+scale+.3),fill=colour)
    pd.rectangle((ox+col0*scale,oy+row0*scale,ox+(col0+ccols)*scale,oy+(row0+crows)*scale),outline='white',width=2)
    pd.text((40,S+12),f'{cols}x{rows} tiles; old square {ccols}x{crows}; ellipse centre + rounded outer ring',fill='white',font=font); pol.save(root/'policy-geometry.png',optimize=True)
    q=json.loads((root/'quality.json').read_text()); c=q['control']['plane_mae']; n=q['colour']['plane_mae']; r=q['round-colour']['plane_mae']
    (root/'comparison.json').write_text(json.dumps({'synthetic':True,'dimensions':[w,h],'frames':8,'crop':{'x':x0,'y':y0,'size':a.crop},'policy_tiles':{'cols':cols,'rows':rows,'centre_cols':ccols,'centre_rows':crows,'centre_origin':[col0,row0]},'mae_control_colour_delta':[n[i]-c[i] for i in range(3)],'mae_control_round_colour_delta':[r[i]-c[i] for i in range(3)],'bytes':{k:q[k]['bytes'] for k in q}},indent=2)+'\n')
if __name__=='__main__': main()
