from pathlib import Path
import subprocess,os
p=Path(__file__).parent;env=os.environ.copy();env.pop('NXVC_PLANAR_CADENCE',None);env['NXVC_PLANAR_WIDE_RING']='1'
for kind in ['static','motion']:
 with (p/f'{kind}-encode.log').open('w') as f:
  subprocess.run(['nx-warp/build-vk/bin/nxvc-vkenc-api','--in',f'nx-scratch/exact-reuse/{kind}.yuv','--out',str(p/f'{kind}.nxv'),'--w','4352','--h','2176','--eyes','2','--frames','16','--qp','40','--inter','--planar-gpu-centre','--centre-quarter','--centre-graduated','--entropy','lite'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
