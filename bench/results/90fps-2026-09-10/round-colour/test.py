from pathlib import Path
import argparse
import numpy as np,subprocess,os,json
ap=argparse.ArgumentParser(); ap.add_argument('--root',type=Path,default=Path(__file__).parent)
ap.add_argument('--encoder',type=Path,default=Path('build-vk/bin/nxvc-vkenc-api'))
ap.add_argument('--decoder',type=Path,default=Path('build-vk/bin/nxv-dec'))
ap.add_argument('--width',type=int,default=4352); ap.add_argument('--height',type=int,default=2176)
ap.add_argument('--frames',type=int,default=8); a=ap.parse_args()
p=a.root; w,h,n=a.width,a.height,a.frames
enc= a.encoder if a.encoder.is_absolute() else Path.cwd()/a.encoder
dec= a.decoder if a.decoder.is_absolute() else Path.cwd()/a.decoder
y,x=np.indices((h,w));Y=(112+32*((x//8+y//8)%2)*(y<h//2)).astype('uint8');yc,xc=np.indices((h//2,w//2));U=np.where((xc//4+yc//4)%2,192,64).astype('uint8');V=np.where((xc//4+yc//4)%2,80,176).astype('uint8')
with (p/'source.yuv').open('wb') as f:
 for t in range(n):
  for a in [Y,U,V]:f.write(np.roll(a,t*4,axis=1).tobytes())
results={}
for name,colour,round_ in [('control',0,0),('colour',1,0),('round-colour',1,1)]:
 env=os.environ.copy();env.update(NXVC_PLANAR_WIDE_RING='1',NXVC_PLANAR_COLOUR=str(colour),NXVC_PLANAR_ROUND=str(round_));env.pop('NXVC_PLANAR_CADENCE',None)
 args=[str(enc),'--in',str(p/'source.yuv'),'--out',str(p/(name+'.nxv')),'--w',str(w),'--h',str(h),'--eyes','2','--frames',str(n),'--qp','40','--inter','--planar-gpu-centre','--centre-quarter','--centre-graduated','--entropy','lite']
 with (p/(name+'-encode.log')).open('w') as f:subprocess.run(args,env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
 with (p/(name+'-decode.log')).open('w') as f:subprocess.run([str(dec),'--in',str(p/(name+'.nxv')),'--out',str(p/(name+'.yuv')),'--pix','yuv420p','--quiet'],stdout=f,stderr=subprocess.STDOUT,check=True)
 a=np.memmap(p/(name+'.yuv'),dtype='uint8',mode='r');src=np.memmap(p/'source.yuv',dtype='uint8',mode='r');assert len(a)==len(src)
 sizes=[w*h,w*h//4,w*h//4];errs=[];cursor=0
 for t in range(n):
  for plane,size in enumerate(sizes):
   errs.append((plane,float(np.abs(a[cursor:cursor+size].astype('int16')-src[cursor:cursor+size].astype('int16')).mean())));cursor+=size
 results[name]={'bytes':(p/(name+'.nxv')).stat().st_size,'plane_mae':[float(np.mean([e for j,e in errs if j==i])) for i in range(3)]};print(name,results[name],flush=True)
(p/'quality.json').write_text(json.dumps(results,indent=2)+'\n')
