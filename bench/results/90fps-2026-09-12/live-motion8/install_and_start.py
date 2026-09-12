#!/usr/bin/env python3
"""Install the matched Pico client and launch the owned 100% HEVC motion profile."""
import argparse,json,os,signal,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--grid',choices=['8','16','32','64'],default='8');p.add_argument('--check',action='store_true');a=p.parse_args()
r=Path(__file__).resolve().parent
adb='/home/nerdrx/.local/bin/adb'
server=Path('/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-atlas-live-current-build/server/wivrn-server')
pkg='org.meumeu.wivrn.nx.warp'
def run(*args): return subprocess.check_output(list(args),text=True).strip()
devices=[line.split()[0] for line in run(adb,'devices').splitlines()[1:] if line.endswith('\tdevice')]
print('Connected Android devices:',len(devices),'Requested motion spacing:',a.grid)
if a.check: raise SystemExit(0)
if len(devices)!=1: raise SystemExit('Connect the Pico through ADB first; no device changed.')
base=[adb,'-s',devices[0]]
manufacturer=run(*base,'shell','getprop','ro.product.manufacturer').lower()
model=run(*base,'shell','getprop','ro.product.model')
if 'pico' not in manufacturer and not model.startswith('A8'):raise SystemExit('Connected device is not the expected Pico; no device changed.')
if not (r/'client.apk').is_file():raise SystemExit('Missing prepared client.apk')
run(*base,'shell','am','force-stop',pkg)
print(run(*base,'install','-r',str(r/'client.apk')))
for key,value in {'motion_mode':'headset','motion_ema':'1' if a.grid!='64' else '0','motion_cap':'1','motion_blur':'0','motion_retain4':'1','motion_past':'1','motion_source_clock':'0','test_eye_size':'0','jit_max_sleep_us':'5000','motion_trace':'0'}.items():
 run(*base,'shell','setprop','debug.wivrn.nx.'+key,value)
run(*base,'shell','setprop','debug.wivrn.jit','1')
pidfile=Path('/run/media/nerdrx/Lex/claude/nx-scratch/motion-live/server-current.pid')
if pidfile.exists():
 old=int(pidfile.read_text());proc=Path('/proc')/str(old)
 if proc.exists():
  argv=[x.decode() for x in (proc/'cmdline').read_bytes().split(b'\0') if x]
  if not argv or Path(argv[0]).resolve()!=server.resolve() or os.getpgid(old)!=old:raise SystemExit('Existing PID is not this owned server; leave it untouched.')
  os.killpg(old,signal.SIGTERM)
  for _ in range(100):
   if not proc.exists():break
   time.sleep(.05)
  else:raise SystemExit('Previous server is still stopping; no duplicate started.')
env=os.environ.copy();env.update(WIVRN_NX_MOTION_BLOCK_PX=a.grid,WIVRN_NX_SOURCE_FPS='60',WIVRN_NX_ALWAYS_MOTION_FIELD='1')
log=Path('/run/media/nerdrx/Lex/claude/nx-scratch/peripheral-smoothing/copy-baseline-server.log').open('ab')
child=subprocess.Popen([str(server),'-f',str(r/'hevc-100-motion8.json')],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
pidfile.write_text(str(child.pid)+'\n');time.sleep(1)
if child.poll() is not None:raise SystemExit('Server exited; inspect server log.')
route=json.loads(run('ip','-j','route','get','192.168.1.1'))[0];address=route.get('prefsrc')
if not address:raise SystemExit('Server ready, but local address is unavailable; connect in the headset.')
run(*base,'shell','input','keyevent','KEYCODE_WAKEUP')
print(run(*base,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://'+address,pkg))
print('Launched server',child.pid,'at',address,'with',a.grid,'px motion cells. Verify received grid and motion in headset.')
