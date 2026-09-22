#!/usr/bin/env python3
"""Continuous radial atlas prototype; CPU reference only."""
import argparse, json, math, struct
from pathlib import Path
import numpy as np
from PIL import Image
from direct_blocks import fixture, block_encode, unpack565


def sample(im, x, y):
    h, w = im.shape[:2]; x = np.clip(x, 0, w - 1); y = np.clip(y, 0, h - 1)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int)
    x1 = np.minimum(x0 + 1, w - 1); y1 = np.minimum(y0 + 1, h - 1)
    wx = (x - x0)[..., None]; wy = (y - y0)[..., None]
    return ((im[y0, x0] * (1-wx) * (1-wy) + im[y0, x1] * wx * (1-wy) +
             im[y1, x0] * (1-wx) * wy + im[y1, x1] * wx * wy)).astype(np.uint8)


def foveate(im, core, tail):
    h, w = im.shape[:2]; yy, xx = np.mgrid[0:h, 0:w]; p = np.stack(((xx/(w-1)*2-1), (yy/(h-1)*2-1)), -1)
    r = np.linalg.norm(p, axis=-1); q = np.where(r[..., None] <= 1e-9, 0, p *
        (np.where(r <= core, r, core + tail*np.tanh((r-core)/tail)) / np.maximum(r, 1e-9))[..., None])
    extent = core + tail * np.tanh((1-core)/tail)
    return q, extent


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--size', type=int, default=1024); ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--core', type=float, default=.42); ap.add_argument('--tail', type=float, default=.25)
    a = ap.parse_args(); assert a.size > 0 and a.size % 32 == 0 and 0 < a.core < 1 and a.tail > 0
    one = fixture(a.size, a.size); src = np.concatenate((one, one[:, ::-1]), 1); q, extent = foveate(one, a.core, a.tail)
    adim = int(math.ceil(a.size * extent / 8) * 8); blocks=[]; dec=[]
    for eye in (one, one[:, ::-1]):
        gy, gx = np.mgrid[0:adim, 0:adim]; u = (gx/(adim-1)*2-1)*extent; v=(gy/(adim-1)*2-1)*extent; rr=np.hypot(u,v)
        pp=np.where(rr<=a.core,rr,a.core+a.tail*np.arctanh(np.clip((rr-a.core)/a.tail,0,.999999)))
        sx=(u/np.maximum(rr,1e-9)*pp+1)*.5*(a.size-1); sy=(v/np.maximum(rr,1e-9)*pp+1)*.5*(a.size-1); atlas=sample(eye,sx,sy); out=np.zeros_like(atlas)
        for y in range(0,adim,8):
            for x in range(0,adim,8):
                raw,_=block_encode(atlas[y:y+8,x:x+8]); blocks.append(raw); w=struct.unpack('<5I',raw); p0=w[0]&65535;p1=w[0]>>16; pal=np.stack([((3-i)*unpack565(p0)+i*unpack565(p1)+1)//3 for i in range(4)]); ids=np.array([(w[1+j//16]>>(2*(j%16)))&3 for j in range(64)]); out[y:y+8,x:x+8]=pal[ids].reshape(8,8,3)
        dec.append(out)
    decoded=[]
    for eye_i in (0,1):
        atlas=dec[eye_i]; gy,gx=np.mgrid[0:a.size,0:a.size]; u=(gx/(a.size-1)*2-1);v=(gy/(a.size-1)*2-1); rr=np.hypot(u,v); qq=np.where(rr<=a.core,rr,a.core+a.tail*np.tanh((rr-a.core)/a.tail)); decoded.append(sample(atlas,(u/np.maximum(rr,1e-9)*qq/extent+1)*.5*(adim-1),(v/np.maximum(rr,1e-9)*qq/extent+1)*.5*(adim-1)))
    dst=np.concatenate(decoded,1); payload=b''.join(blocks); a.out.mkdir(parents=True,exist_ok=True); Image.fromarray(src).save(a.out/'source.png'); Image.fromarray(dst).save(a.out/'decoded.png'); (a.out/'blocks.bin').write_bytes(payload); np.concatenate((dst,np.full((*dst.shape[:2],1),255,np.uint8)),2).tofile(a.out/'cpu.rgba')
    mse=np.mean((src.astype(float)-dst.astype(float))**2); meta={'format':'direct-radial-atlas-v1','display_dimensions':[2*a.size,a.size],'atlas_eye_dimension':adim,'core_radius':a.core,'tail':a.tail,'extent':extent,'block_bytes':20,'payload_bytes':len(payload),'stereo_payload_bitrate_mbps':len(payload)*8*90/1e6,'psnr_db_cpu_reference':float(10*math.log10(255**2/mse)),'realtime_or_gpu_validated':False,'atlas_layout':'two eyes row-major, 8x8 blocks, no descriptors'}; (a.out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')

if __name__=='__main__': main()
