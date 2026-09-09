"""GPU cadence regression: requires numpy and a built NX Warp tools directory."""
import os,sys,subprocess,json,re
from pathlib import Path
import numpy as np
root=Path(__file__).resolve().parent
bins=Path(sys.argv[1]).resolve()
def call(args,log,env=None):
 with (root/log).open('w') as f:subprocess.run([str(a) for a in args],env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
w,h,n=1024,512,16;y,x=np.indices((h,w));Y=((x//16*13+y//16*7)%200+28).astype('uint8');C=np.full((h//2,w//2),128,dtype='uint8')
with (root/'motion.yuv').open('wb') as f:
 for t in range(n):
  a=Y.copy();a[128:384,192:320]=(a[128:384,192:320].astype('uint16')+t*11)%256;a[32:160,32+t*8:160+t*8]=230
  f.write(a.tobytes()+C.tobytes()+C.tobytes())
(root/'static.yuv').write_bytes((Y.tobytes()+C.tobytes()+C.tobytes())*n)
for label,src,extra,on in [('off','motion',[],False),('on','motion',[],True),('on-qp','motion',['--qp-cycle','22,40,30,36'],True),('on-static','static',[],True)]:
 env=os.environ.copy();env.pop('NXVC_PLANAR_CADENCE',None)
 if on:env.update(NXVC_PLANAR_CADENCE='1',NXVC_PLANAR_CADENCE_TRACE='1')
 call([bins/'nxvc-vkenc-api','--in',root/(src+'.yuv'),'--w',w,'--h',h,'--eyes',2,'--out',root/(label+'.nxv'),'--frames',n,'--qp',40,'--inter','--planar-gpu-centre','--centre-quarter','--centre-graduated',*extra],label+'.log',env)
for label in ['old','off','on','old-qp','on-qp','old-static','on-static']:
 call([bins/'nxv-dec','--in',root/(label+'.nxv'),'--out',root/(label+'-decoded.yuv'),'--pix','yuv420p','--quiet'],label+'-decode.log')
checks={}
for a,b in [('old','off'),('old-qp','on-qp'),('old-static','on-static')]:
 checks[a+'='+b]=(root/(a+'.nxv')).read_bytes()==(root/(b+'.nxv')).read_bytes();assert checks[a+'='+b]
a=np.fromfile(root/'old-decoded.yuv',dtype='uint8').reshape(n,-1);b=np.fromfile(root/'on-decoded.yuv',dtype='uint8').reshape(n,-1)
for offset,ph,pw in [(0,h,w),(h*w,h//2,w//2),(h*w*5//4,h//2,w//2)]:
 aa=a[:,offset:offset+ph*pw].reshape(n,ph,pw);bb=b[:,offset:offset+ph*pw].reshape(n,ph,pw)
 for eye in range(2):
  x0=eye*pw//2+3*pw//16;y0=3*ph//8
  assert np.array_equal(aa[:,y0:y0+ph//4,x0:x0+pw//8],bb[:,y0:y0+ph//4,x0:x0+pw//8])
checks['centre_exact']=True;checks['changed_samples']=int((a!=b).sum())
call([bins/'nxv-dec','--in',root/'on.nxv','--out',root/'drop.yuv','--pix','yuv420p','--decode-every',2,'--quiet'],'drop.log')
assert np.array_equal(b[::2],np.fromfile(root/'drop.yuv',dtype='uint8').reshape(n//2,-1));checks['dropped_frames_exact']=True
call([bins/'nxvc-vkdec','--in',root/'on.nxv','--out',root/'vk.yuv','--pix','yuv420p','--format','ycbcr420'],'vk.log')
assert (root/'vk.yuv').read_bytes()==(root/'on-decoded.yuv').read_bytes();checks['vulkan_cpu_exact']=True
rows=[list(map(int,m)) for m in re.findall(r'cadence: frame (\d+) fit (\d+) reuse (\d+) hot (\d+) max_age (\d+)',(root/'on.log').read_text())]
assert len(rows)==n and max(r[4] for r in rows)<=3 and any(r[2]>0 for r in rows) and any(r[3]>0 for r in rows)
checks['cadence']=rows
(root/'checks.json').write_text(json.dumps(checks,indent=2)+'\n');print({k:v for k,v in checks.items() if k!='cadence'})
