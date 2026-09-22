"""Integer bilinear reconstruction oracle for the coarse regions only."""
import argparse
from pathlib import Path
import numpy as np
from PIL import Image

def soften(rgb, descriptors, tile_clamp=False):
    h,w=rgb.shape[:2]
    result=rgb.copy()
    # Row chunks limit memory when validating headset-sized stereo images.
    for y0 in range(0,h,64):
        y,x=np.mgrid[y0:min(y0+64,h),:w]
        d=descriptors[(y//32)*(w//32)+x//32]
        scale=1 << (d>>30)
        den=2*scale
        px,py=2*x+1-scale,2*y+1-scale
        bx,by=px//den,py//den
        fx,fy=px-bx*den,py-by*den
        total=np.zeros((*x.shape,3),dtype=np.int64)
        eye=(x//(w//2))*(w//2)
        for j in range(2):
            for i in range(2):
                sx=np.clip((bx+i)*scale,eye,eye+w//2-1)
                sy=np.clip((by+j)*scale,0,h-1)
                if tile_clamp:
                    sx=np.clip(sx,(x//32)*32,(x//32)*32+31)
                    sy=np.clip(sy,(y//32)*32,(y//32)*32+31)
                weight=np.where(i,fx,den-fx)*np.where(j,fy,den-fy)
                total+=rgb[sy,sx].astype(np.int64)*weight[...,None]
        out=(total+(den*den//2)[...,None])//(den*den)[...,None]
        result[y0:y0+x.shape[0]]=out.astype(np.uint8)
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('fixture',type=Path);p.add_argument('output',type=Path);p.add_argument('--tile-clamp',action='store_true');a=p.parse_args()
    rgb=np.asarray(Image.open(a.fixture/'decoded.png').convert('RGB'))
    d=np.fromfile(a.fixture/'descriptors.bin',dtype='<u4')
    dst=soften(rgb,d,a.tile_clamp)
    Image.fromarray(dst).save(a.output.with_suffix('.png'))
    np.concatenate((dst,np.full((*dst.shape[:2],1),255,dtype=np.uint8)),axis=2).tofile(a.output)
