from pathlib import Path
import subprocess,os,json,hashlib
r=Path('/run/media/nerdrx/Lex/claude');o=r/'nx-scratch/native-tskip';b=r/'nx-scratch/nxwarp-atlas-live-build/bin'
env=os.environ.copy();env.update(NXVC_PLANAR_LARGE_CENTRE='1',NXVC_PLANAR_ROUND='1',NXVC_PLANAR_COLOUR='1',NXVC_PLANAR_WIDE_RING='1',NXVC_PLANAR_NO_GROUPS='1');env.pop('NXVC_PLANAR_R4',None)
def call(args,e):
 q=subprocess.run(list(map(str,args)),env=e,capture_output=True,text=True);(o/'checks.log').open('a').write(' '.join(map(str,args))+chr(10)+q.stdout+q.stderr);q.check_returncode()
res={}
for mode in ['base','skip']:
 e=env.copy();e['NXVC_NATIVE_TRANSFORM_SKIP']='1' if mode=='skip' else '0';stream=o/(mode+'.nxv')
 call([b/'nxvc-vkenc-api','--in',r/'nx-scratch/large-centre/fixture.yuv','--w','5376','--h','2688','--eyes','2','--frames','1','--qp','40','--entropy','lite','--inter','--intra-period','1','--planar-gpu-centre','--centre-quarter','--centre-graduated','--out',stream],e)
 call([b/'nxv-dec','--in',stream,'--out',o/(mode+'-cpu.nv12'),'--pix','yuv420p','--nv12'],e)
 e['NXVC_VKD_PLANAR_FLAT']='1'
 call([b/'nxvc-vkdec','--in',stream,'--out',o/(mode+'-gpu.nv12'),'--format','ycbcr420','--nv12','--unorm','0','--stats'],e)
 cpu=(o/(mode+'-cpu.nv12')).read_bytes();gpu=(o/(mode+'-gpu.nv12')).read_bytes();assert cpu==gpu,mode+' cpu/gpu mismatch'
 res[mode]={'stream_bytes':stream.stat().st_size,'cpu_gpu_exact':True,'output_sha256':hashlib.sha256(cpu).hexdigest()}
(o/'results.json').write_text(json.dumps(res,indent=2));print(json.dumps(res))
