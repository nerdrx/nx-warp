#!/usr/bin/env python3
"""Emit GPU encoder jobs and CPU expected stream for direct_blocks.comp."""
import argparse, json, struct
from pathlib import Path
import numpy as np
from PIL import Image
from direct_blocks import fixture, encode

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--size',type=int,default=2048); ap.add_argument('--out',type=Path,required=True); a=ap.parse_args()
    assert a.size > 0 and a.size % 32 == 0
    one=fixture(a.size,a.size); src=np.concatenate((one,one[:,::-1]),1); desc,blocks,_=encode(src)
    words=struct.unpack('<%dI'%(len(desc)//4),desc); tw=src.shape[1]//32; jobs=[]
    for ti,d in enumerate(words):
        ty,tx=divmod(ti,tw); scale=1<<((d>>30)&3); n=32//scale; count=(n//8)**2
        for k in range(count):
            by,bx=divmod(k,n//8); jobs.append((tx*32+bx*8*scale,ty*32+by*8*scale,scale,(d&0x3fffffff)+k*5))
    jobs.sort(key=lambda job: job[3])
    a.out.mkdir(parents=True,exist_ok=True); Image.fromarray(src).save(a.out/'source.png'); np.concatenate((src,np.full((*src.shape[:2],1),255,np.uint8)),2).tofile(a.out/'source.rgba'); (a.out/'jobs.bin').write_bytes(struct.pack('<%dI'%(len(jobs)*4),*(v for j in jobs for v in j))); (a.out/'expectedblocks.bin').write_bytes(blocks); (a.out/'descriptors.bin').write_bytes(desc)
    (a.out/'metadata.json').write_text(json.dumps({'dimensions':list(src.shape[:2][::-1]),'job_count':len(jobs),'block_count':len(blocks)//20,'source_format':'packed RGBA8 uint32 words','job_layout':'uvec4(originX,originY,scale,outputWordOffset)','expected':'CPU direct_blocks reference'},indent=2)+'\n')
if __name__=='__main__': main()
