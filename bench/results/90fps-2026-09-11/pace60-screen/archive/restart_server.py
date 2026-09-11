#!/usr/bin/env python3
"""Restart only the owned WiVRn server process group, preserving its environment."""
import os,signal,subprocess,sys,time
from pathlib import Path
root=Path('/run/media/nerdrx/Lex/claude');pid=int(sys.argv[1]);proc=Path('/proc')/str(pid)
args=[x.decode() for x in (proc/'cmdline').read_bytes().split(b'\0') if x]
if not args or Path(args[0]).name!='wivrn-server':raise RuntimeError('Unexpected server executable')
if os.getpgid(pid)!=pid:raise RuntimeError('Server does not own its process group')
env={k.decode():v.decode() for raw in (proc/'environ').read_bytes().split(b'\0') if b'=' in raw for k,v in [raw.split(b'=',1)]}
if '--pace-accumulate' in sys.argv:env['NXWARP_PACE_ACCUMULATE']='1'
if '--pace-legacy' in sys.argv:env.pop('NXWARP_PACE_ACCUMULATE',None)
if '--wide-ring' in sys.argv:env['NXVC_PLANAR_WIDE_RING']='1'
if '--default-ring' in sys.argv:env.pop('NXVC_PLANAR_WIDE_RING',None)
if '--timings' in sys.argv:env['WIVRN_DUMP_TIMINGS']=str(Path(sys.argv[sys.argv.index('--timings')+1]).resolve())
if '--no-timings' in sys.argv:env.pop('WIVRN_DUMP_TIMINGS',None)
for flag,key in [('--round','NXVC_PLANAR_ROUND'),('--colour','NXVC_PLANAR_COLOUR'),('--large-centre','NXVC_PLANAR_LARGE_CENTRE'),('--no-groups','NXVC_PLANAR_NO_GROUPS'),('--r4','NXVC_PLANAR_R4'),('--native-tskip','NXVC_NATIVE_TRANSFORM_SKIP')]:
 if flag in sys.argv:env[key]='1'
 if flag+'-off' in sys.argv:env.pop(key,None)
if '--config' in sys.argv:
 config=Path(sys.argv[sys.argv.index('--config')+1]).resolve()
 if not config.is_file():raise RuntimeError('Missing configuration')
 if '-f' not in args:raise RuntimeError('Expected explicit configuration argument')
 args[args.index('-f')+1]=str(config)
cwd=os.readlink(proc/'cwd')
subprocess.run(['/home/nerdrx/.local/bin/adb','shell','am','force-stop','org.meumeu.wivrn.nx.warp'],check=True)
os.killpg(pid,signal.SIGTERM)
for _ in range(100):
 if not proc.exists():break
 time.sleep(.05)
else:
 if '--force-owned' not in sys.argv:raise RuntimeError('Old server has not exited; no duplicate started')
 os.killpg(pid,signal.SIGKILL)
 time.sleep(.5)
log=(root/'nx-scratch/peripheral-smoothing/copy-baseline-server.log').open('ab')
p=subprocess.Popen(args,cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
(root/'nx-scratch/motion-live/server-current.pid').write_text(str(p.pid)+'\n')
time.sleep(1)
if p.poll() is not None:raise RuntimeError('New server exited early')
print('Started WiVRn server PID',p.pid)
