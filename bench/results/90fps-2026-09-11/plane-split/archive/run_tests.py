from pathlib import Path
import subprocess,hashlib,json,os,time
root=Path('/run/media/nerdrx/Lex/claude'); p=root/'nx-scratch/r4-palette'; b=root/'nx-scratch/nxwarp-atlas-live-build/bin'; out=root/'nx-scratch/plane-split'; out.mkdir(exist_ok=True)
results={}
for kind in ('r2','r4'):
  inp=p/f'candidate-{kind}.nxv'
  for split in (0,1):
    env=os.environ.copy(); env['NXVC_VKD_PLANAR_FLAT']='1'; env['NXVC_VKD_NATIVE_PLANE_SPLIT']='1' if split else '0'
    dst=out/f'{kind}-split{split}.nv12'; log=out/f'{kind}-split{split}.log'
    cmd=[b/'nxvc-vkdec','--in',inp,'--out',dst,'--format','ycbcr420','--nv12','--compact-large-centre','--compact-flat64','--independent-tiles','--unorm','0','--stats']
    t=time.time(); q=subprocess.run(list(map(str,cmd)),env=env,capture_output=True,text=True,timeout=90); elapsed=time.time()-t
    log.write_text(q.stdout+q.stderr)
    rec={'returncode':q.returncode,'seconds':elapsed,'bytes':dst.stat().st_size if dst.exists() else None,'sha256':hashlib.sha256(dst.read_bytes()).hexdigest() if dst.exists() else None,'dispatch_lines':[x for x in (q.stdout+q.stderr).splitlines() if 'dispatch' in x.lower() or 'passB' in x.lower()]}
    results[f'{kind}_split{split}']=rec
  a=(out/f'{kind}-split0.nv12').read_bytes(); c=(out/f'{kind}-split1.nv12').read_bytes()
  results[f'{kind}_equal']=(a==c)
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
