#!/usr/bin/env python3
"""Validate the direct PLANAR probe against the reference decoder.

The probe writes <prefix>.rgba and <prefix>.ppm.  This harness deliberately
clears NX_PLANAR_* from the environment and treats every unexpected command
failure, missing output, geometry mismatch, or pixel mismatch as a failure.
"""
import argparse, json, os, pathlib, re, subprocess, sys

def run(cmd, env, log):
    p = subprocess.run(cmd, env=env, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    log.write_text(p.stdout)
    return p

def ppm(path):
    b = pathlib.Path(path).read_bytes(); parts=b.split(b'\n',3)
    if len(parts)!=4 or parts[0]!=b'P6' or parts[3].__len__()==0: raise ValueError('bad PPM')
    w,h=map(int,parts[1].split()); return w,h,parts[3]

def yuv_rgba(yuv,w,h):
    ysz=w*h; csz=(w//2)*(h//2); need=ysz+2*csz
    if len(yuv)!=need: raise ValueError(f'YUV length {len(yuv)} != {need}')
    y=yuv[:ysz]; cb=yuv[ysz:ysz+csz]; cr=yuv[ysz+csz:]
    out=bytearray(ysz*4)
    for py in range(h):
      for px in range(w):
        i=py*w+px; c=(py//2)*(w//2)+(px//2)
        out[4*i:4*i+4]=bytes((y[i],cb[c],cr[c],255))
    return out

def one(args, stream, mode, root, env):
    tag=stream.stem.replace(',','_')+'-'+mode
    prefix=root/tag
    e=env.copy()
    if mode=='flat': e['NX_PLANAR_FLAT']='1'
    if mode=='compact': e['NX_PLANAR_COMPACT']='1'
    log=root/(tag+'.log')
    p=run([args.probe,str(stream),args.shaders,'2',str(prefix)],e,log)
    if p.returncode: return {'status':'probe_failed','returncode':p.returncode,'log':log.name}
    w,h,rawppm=ppm(str(prefix)+'.ppm')
    rgba=(prefix.with_suffix('.rgba')).read_bytes()
    if len(rgba)!=w*h*4: return {'status':'bad_rgba_length','width':w,'height':h}
    if mode != 'exact':
        return {'status':'ok','width':w,'height':h}
    refout=root/(tag+'.ref.yuv')
    rp=run([args.ref_decoder,'--in',str(stream),'--out',str(refout),'--frames','2','--pix','yuv420p'],env,root/(tag+'.ref.log'))
    if rp.returncode: return {'status':'ref_failed','returncode':rp.returncode}
    ref=refout.read_bytes(); frame=w*h+2*(w//2)*(h//2)
    if len(ref)<2*frame: return {'status':'ref_short','bytes':len(ref),'frame_bytes':frame}
    expected=yuv_rgba(ref[-frame:],w,h)
    md=max(abs(a-b) for a,b in zip(rgba,expected))
    return {'status':'ok' if md==0 else 'pixel_mismatch','width':w,'height':h,'maxdiff':md}

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--probe',required=True); ap.add_argument('--shaders',required=True)
    ap.add_argument('--ref-decoder',required=True); ap.add_argument('--fixtures-dir',required=True); ap.add_argument('--out',required=True)
    ap.add_argument('--malformed',action='append',default=[]); ap.add_argument('--nonplanar',action='append',default=[]); args=ap.parse_args()
    out=pathlib.Path(args.out); out.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy()
    for k in list(env):
        if k.startswith('NX_PLANAR_'): env.pop(k)
    fixtures=sorted(pathlib.Path(args.fixtures_dir).glob('*.nxv'))
    if not fixtures: print('no fixtures',file=sys.stderr); return 2
    results=[]
    for s in fixtures: results.append({'fixture':s.name,'mode':'exact',**one(args,s,'exact',out,env)})
    # The compact shader intentionally supports only two coarse regions (R2).
    for s in fixtures:
        x=one(args,s,'flat',out,env)
        results.append({'fixture':s.name,'mode':'flat',**x})
        c=one(args,s,'compact',out,env)
        m = re.search(r'-(\d+),', s.stem)
        r2 = s.stem.endswith('-2,0')
        if r2:
            results.append({'fixture':s.name,'mode':'compact',**c})
        elif c.get('status')=='probe_failed':
            results.append({'fixture':s.name,'mode':'compact','status':'expected_rejection','returncode':c.get('returncode')})
        else:
            results.append({'fixture':s.name,'mode':'compact',**c,'status':'unexpected_acceptance'})
        if r2 and x.get('status')=='ok' and c.get('status')=='ok':
            xf=(out/(s.stem.replace(',','_')+'-flat.rgba')).read_bytes()
            cf=(out/(s.stem.replace(',','_')+'-compact.rgba')).read_bytes()
            md=max(abs(a-b) for a,b in zip(xf,cf)) if len(xf)==len(cf) else None
            results.append({'fixture':s.name,'mode':'flat_vs_compact','status':'ok' if md==0 else 'pixel_mismatch','maxdiff':md})
    for name in args.malformed+args.nonplanar:
        s=pathlib.Path(name); log=out/(s.stem+'-reject.log')
        p=run([args.probe,str(s),args.shaders,'2'],env,log)
        results.append({'fixture':str(s),'mode':'reject','status':'rejected' if p.returncode else 'accepted','returncode':p.returncode})
    (out/'validation.json').write_text(json.dumps(results,indent=2)+'\n')
    bad=[r for r in results if r['status'] not in ('ok','rejected','expected_rejection')]
    if bad: print(json.dumps(bad,indent=2),file=sys.stderr); return 1
    print(json.dumps(results,indent=2)); return 0
if __name__=='__main__': sys.exit(main())
