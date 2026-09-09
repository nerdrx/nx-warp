"""Run on an idle Pico: adb, Android decoder, stream, host reference decoder."""
import hashlib,json,re,subprocess,sys
from pathlib import Path
import numpy as np
p=Path(__file__).resolve().parent
adb,binary,stream,ref=sys.argv[1:]
def call(args,log):
 with (p/log).open('w') as f: subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,check=True)
call([adb,'push',binary,'/data/local/tmp/nxvc-flat-wg'],'push-binary.log')
call([adb,'push',stream,'/data/local/tmp/flat-wg.nxv'],'push-stream.log')
call([adb,'shell','chmod','755','/data/local/tmp/nxvc-flat-wg'],'chmod.log')
base=['/data/local/tmp/nxvc-flat-wg','--in','/data/local/tmp/flat-wg.nxv','--format','ycbcr420','--compact-centre','--unorm','1']
hashes={}
for wg in [256,128,64]:
 call([adb,'shell','env',f'NXVC_VKD_FLAT_WG={wg}',*base,'--out','/data/local/tmp/flat-wg.yuv'],f'pixels-{wg}.log')
 call([adb,'pull','/data/local/tmp/flat-wg.yuv',str(p/f'{wg}.yuv')],f'pull-{wg}.log')
 hashes[str(wg)]=hashlib.sha256((p/f'{wg}.yuv').read_bytes()).hexdigest()
assert len(set(hashes.values()))==1,hashes
# Independent reference: first full-resolution CPU-decoded frame, exact retained samples.
call([ref,'--in',stream,'--out',str(p/'cpu.yuv'),'--frames','1','--pix','yuv420p','--quiet'],'cpu.log')
raw=np.fromfile(p/'cpu.yuv',dtype=np.uint8);parts=[];cursor=0
for size in [64,32,32]:
 h=34*size;w=68*size;a=raw[cursor:cursor+h*w].reshape(h,w);cursor+=h*w
 axis=np.concatenate([np.arange(t*size,(t+1)*size) if 13<=t<21 else np.arange(t*size+1,(t+1)*size,4) for t in range(34)])
 xx=np.concatenate([axis,axis+34*size]);parts.append(a[np.ix_(axis,xx)].tobytes())
expected=b''.join(parts)
assert (p/'256.yuv').read_bytes()[:len(expected)]==expected,'CPU retained-sample mismatch'
rows=[]
for run,wg in enumerate([256,64,128,256,128,64,256]):
 log=f'perf-{run}-{wg}.log'
 call([adb,'shell','env',f'NXVC_VKD_FLAT_WG={wg}',*base,'--no-out','--stats'],log)
 times=[list(map(float,m)) for m in re.findall(r'passA ([\d.]+)\s+passW ([\d.]+)\s+passB ([\d.]+)\s+gpu ([\d.]+)',(p/log).read_text())][4:]
 assert len(times)>=28,(log,len(times))
 v=np.array(times)
 rows.append({'wg':wg,'run':run,'frames':len(v),'passA_mean_ms':float(v[:,0].mean()),'passB_mean_ms':float(v[:,2].mean()),'gpu_mean_ms':float(v[:,3].mean()),'gpu_p95_ms':float(np.percentile(v[:,3],95))})
result={'pixel_hashes':hashes,'cpu_first_frame_exact':True,'runs':rows,'binary_sha256':hashlib.sha256(Path(binary).read_bytes()).hexdigest(),'stream_sha256':hashlib.sha256(Path(stream).read_bytes()).hexdigest()}
(p/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
