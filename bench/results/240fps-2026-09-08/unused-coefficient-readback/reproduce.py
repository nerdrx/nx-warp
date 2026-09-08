#!/usr/bin/env python3
"""Reproduce recorded coefficient-readback byte comparisons.

Runs supplied before/after encoder binaries sequentially; it never selects a
binary implicitly. GPU execution occurs only when this script is invoked.
"""
import argparse, hashlib, json, subprocess, tempfile
from pathlib import Path

def make_yuv(path, w, h, frames=8):
    with path.open('wb') as output:
        for frame in range(frames):
            luma = bytearray([32]) * (w*h)
            if w == 256:
                positions = [16 + frame*8]; top, size = 24, 32
            else:
                positions = [eye*2176 + 320 + frame*48 for eye in range(2)]
                top, size = 640, 256
            for left in positions:
                for row in range(top, top+size):
                    luma[row*w+left:row*w+left+size] = bytes([220])*size
            output.write(luma)
            output.write(bytes([128])*(w*h//2))

def run(binary, yuv, w, h, out, entropy, trellis, log):
    cmd=[binary,'--in',str(yuv),'--w',str(w),'--h',str(h),
         '--pix','yuv420p','--qp','40','--frames','8','--eyes','2',
         '--atlas','--atlas-mode','--inter','--entropy',entropy,'--out',str(out)]
    if trellis: cmd += ['--trellis','1']
    with log.open('w') as f:
        subprocess.run(cmd, check=True, stdout=f, stderr=subprocess.STDOUT)

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--before',required=True); ap.add_argument('--after',required=True); ap.add_argument('--output',type=Path,required=True); a=ap.parse_args(); a.output.mkdir(parents=True,exist_ok=True)
    cases=[('lite-small',256,128,'lite',False),('lite-native',4352,2176,'lite',False),('rans-small',256,128,'rans',False),('lite-trellis',256,128,'lite',True)]
    result=[]
    with tempfile.TemporaryDirectory(prefix='nx-coef-readback-') as td:
      td=Path(td)
      for name,w,h,entropy,trellis in cases:
        yuv=td/(name+'.yuv'); make_yuv(yuv,w,h)
        outputs=[]
        for label,binary in [('before',a.before),('after',a.after)]:
          out=td/(name+'-'+label+'.nxv'); log=a.output/(name+'-'+label+'.log'); run(binary,yuv,w,h,out,entropy,trellis,log); outputs.append(out.read_bytes())
        equal=outputs[0]==outputs[1]; digest=hashlib.sha256(outputs[0]).hexdigest()
        (a.output/(name+'.nxv')).write_bytes(outputs[0]); result.append({'case':name,'sha256':digest,'equal':equal})
        if not equal: raise SystemExit('byte mismatch: '+name)
    (a.output/'validation.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__': main()
