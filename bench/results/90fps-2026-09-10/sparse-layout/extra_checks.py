from pathlib import Path
import subprocess,hashlib,json
p=Path(__file__).parent;a='/home/nerdrx/.local/bin/adb';b='/data/local/tmp/nx-sparse-layout/'
def run(args):return subprocess.run(args,check=True,capture_output=True,text=True)
checks={}
for label,source,extra in [('rans',p.parent/'exact-reuse/motion.nxv',[]),('lite-dense',p.parent/'lite-bitcount/motion.nxv',['--dense'])]:
 run([a,'push',str(source),b+'extra.nxv']);hh=[]
 for mode in ['control','candidate']:
  env=['NXVC_VKD_PLANAR_FLAT=1']+(['NXVC_VKD_PASSA_DYNAMIC_LAYOUT=1'] if mode=='control' else [])
  r=run([a,'shell','env',*env,b+'vkdec','--in',b+'extra.nxv','--out',b+'extra.yuv','--frames','3','--format','ycbcr420','--compact-flat64','--unorm','0',*extra]);(p/f'{label}-{mode}.log').write_text(r.stdout+r.stderr)
  run([a,'pull',b+'extra.yuv',str(p/'extra.yuv')]);hh.append(hashlib.sha256((p/'extra.yuv').read_bytes()).hexdigest())
 assert hh[0]==hh[1];checks[label]=hh;print(label,'exact',flush=True)
(p/'extra-checks.json').write_text(json.dumps(checks,indent=2))
