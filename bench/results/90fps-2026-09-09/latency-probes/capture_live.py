#!/usr/bin/env python3
"""Headless synthetic-motion trial. Preserve logs even if the client dies."""
import json,os,re,signal,subprocess,sys,time
from pathlib import Path
ROOT=Path('/run/media/nerdrx/Lex/claude');OUT=ROOT/'nx-scratch/motion-live'
ADB='/home/nerdrx/.local/bin/adb';PKG='org.meumeu.wivrn.nx.warp'
label,fdm,wait=sys.argv[1:4];duration=int(sys.argv[4]) if len(sys.argv)>4 else 90
capture=sys.argv[5] if len(sys.argv)>5 else '0'
children=[];pid=None;started=time.monotonic()
server=ROOT/'nx-scratch/peripheral-smoothing/copy-baseline-server.log';offset=server.stat().st_size
status={'fdm':int(fdm),'wait_us':int(wait),'requested_seconds':duration,'capture':capture,'scene':'NXWARP_BENCH_FULL_FIELD'}
def adb(*args):return subprocess.check_output([ADB,*args],text=True).strip()
def start(name,args,env=None):
 f=(OUT/f'{label}-{name}.log').open('w');p=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT,start_new_session=True,env=env);children.append((p,f));return p
try:
 adb('shell','am','force-stop',PKG)
 for key,value in {'fdm':fdm,'ready_wait_us':wait,'capture':capture,'compact_centre':'1','planar_centre':'1','borrowed_output':'1','peripheral_smooth':'0'}.items():adb('shell','setprop','debug.wivrn.nx.'+key,value)
 adb('shell','setprop','debug.wivrn.jit','1')
 adb('shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2',PKG)
 for _ in range(20):
  try:pid=adb('shell','pidof',PKG).split()[0];break
  except (subprocess.CalledProcessError,IndexError):time.sleep(.1)
 if not pid:raise RuntimeError('Client did not start')
 status['pid']=pid
 time.sleep(3)
 env=os.environ.copy();env['XR_RUNTIME_JSON']=str(ROOT/'nx-scratch/wivrn-atlas-live-current-build/openxr_wivrn-dev.json');env['NXWARP_BENCH_FULL_FIELD']='1'
 binary=ROOT/'nx-scratch/hello-xr-bench-build/src/tests/hello_xr/hello_xr'
 scene=start('scene',['gamescope','--backend','headless','-W','1280','-H','720','--','sh','-c',f'tail -f /dev/null | stdbuf -oL {binary} -g Vulkan2'],env)
 trial_start=time.monotonic();capture_step=0
 while time.monotonic()-trial_start<duration:
  time.sleep(min(2,max(.01,duration-(time.monotonic()-trial_start))))
  if capture != '0' and capture_step < 2 and time.monotonic()-trial_start >= (capture_step+1)*10:
   capture_step += 1
   adb('shell','setprop','debug.wivrn.nx.capture',capture+str(capture_step))
  if 'Unsupported graphics API' in (OUT/f'{label}-scene.log').read_text(errors='replace'):
   raise RuntimeError('Benchmark binary lacks requested graphics backend')
  if scene.poll() is not None:raise RuntimeError('Headless scene exited early')
  if adb('shell','pidof',PKG).split()[0]!=pid:raise RuntimeError('Client process changed')
 if 'NXWARP_BENCH_FULL_FIELD frame ' not in (OUT/f'{label}-scene.log').read_text(errors='replace'):
  raise RuntimeError('Synthetic scene did not report advancing frames')
 times=re.findall(r'NXWARP_BENCH_FULL_FIELD frame \d+ t=([0-9.]+) s', (OUT/f'{label}-scene.log').read_text(errors='replace'))
 status['last_scene_time_s']=float(times[-1]) if times else 0
 if status['last_scene_time_s'] < duration-10:raise RuntimeError('Scene animation did not advance through trial end')
 status['complete']=True
except Exception as e:
 status['complete']=False;status['error']=str(e)
finally:
 status['elapsed_seconds']=time.monotonic()-started
 if pid:
  with (OUT/f'{label}-client.log').open('w') as f:subprocess.run([ADB,'logcat','-d','--pid='+pid,'-v','threadtime','WiVRn:I','PxrMetric:I','*:S'],stdout=f)
 try:status['client_alive']=bool(pid and pid in adb('shell','pidof',PKG).split())
 except subprocess.CalledProcessError:status['client_alive']=False
 if not status.get('complete') and not status['client_alive']:
  with (OUT/f'{label}-crash.log').open('w') as f:subprocess.run([ADB,'logcat','-d','-b','crash','-v','threadtime'],stdout=f)
 for p,f in reversed(children):
  try:os.killpg(p.pid,signal.SIGTERM)
  except ProcessLookupError:pass
  try:p.wait(timeout=5)
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
  f.close()
 with server.open('rb') as f:f.seek(offset);(OUT/f'{label}-server.log').write_bytes(f.read())
 (OUT/f'{label}-status.json').write_text(json.dumps(status,indent=2)+'\n')
 print(json.dumps(status),flush=True)
 if not status.get('complete'):sys.exit(1)
