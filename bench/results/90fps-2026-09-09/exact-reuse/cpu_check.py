from pathlib import Path
import subprocess,numpy as np,json
p=Path(__file__).parent
checks={}
for kind in ['static','motion','qp']:
 with (p/f'cpu-{kind}.log').open('w') as f:
  subprocess.run(['nx-warp/build-vk/bin/nxv-dec','--in',str(p/f'{kind}.nxv'),'--out',str(p/'cpu.yuv'),'--frames','16','--pix','yuv420p','--quiet'],stdout=f,stderr=subprocess.STDOUT,check=True)
 raw=np.memmap(p/'cpu.yuv',dtype=np.uint8,mode='r');gpu=np.memmap(p/f'{kind}-1.yuv',dtype=np.uint8,mode='r');cursor=0;gc=0
 for frame in range(16):
  for size in [64,32,32]:
   h=34*size;w=68*size;a=raw[cursor:cursor+h*w].reshape(h,w);cursor+=h*w
   axis=np.concatenate([np.arange(t*size,(t+1)*size) if 13<=t<21 else np.arange(t*size+1,(t+1)*size,4) for t in range(34)])
   xx=np.concatenate([axis,axis+34*size]);expected=a[np.ix_(axis,xx)].ravel()
   assert np.array_equal(gpu[gc:gc+len(expected)],expected),(kind,frame,size);gc+=len(expected)
 checks[kind]={'cpu_retained_samples_exact':True,'frames':16};print(kind,'CPU exact',flush=True)
(p/'cpu-checks.json').write_text(json.dumps(checks,indent=2)+'\n')
