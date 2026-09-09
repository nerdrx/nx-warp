#!/usr/bin/env python3
"""Capture a bounded headless Pico hello_xr run; does not automate the desktop UI."""
import os,signal,subprocess,time,sys
from pathlib import Path
root=Path('/run/media/nerdrx/Lex/claude');out=root/'nx-scratch/compact-direct';adb='/home/nerdrx/.local/bin/adb'
label,compact=sys.argv[1:3];duration=int(sys.argv[3]) if len(sys.argv)>3 else 90
smooth=sys.argv[4] if len(sys.argv)>4 else '1'
children=[]
def run(*args):subprocess.run([adb,*args],check=True)
def start(name,args,env=None):
 f=(out/(label+'-'+name+'.log')).open('w');q=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT,start_new_session=True,env=env);children.append(q);(out/(label+'-'+name+'.pid')).write_text(str(q.pid));return q
try:
 run('shell','am','force-stop','org.meumeu.wivrn.nx.warp')
 run('shell','setprop','debug.wivrn.nx.compact_centre',compact)
 for key in ['planar_centre','borrowed_output']:run('shell','setprop','debug.wivrn.nx.'+key,'1')
 run('shell','setprop','debug.wivrn.nx.peripheral_smooth',smooth)
 start('client',[adb,'logcat','-T','1','-v','threadtime','WiVRn:I','*:S'])
 run('shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp')
 time.sleep(3)
 env=os.environ.copy();env['XR_RUNTIME_JSON']=str(root/'nx-scratch/wivrn-atlas-live-current-build/openxr_wivrn-dev.json')
 start('hello',['gamescope','--backend','headless','-W','1280','-H','720','--','sh','-c','tail -f /dev/null | hello_xr -g Vulkan2'],env)
 time.sleep(duration)
 pid=subprocess.check_output([adb,'shell','pidof','org.meumeu.wivrn.nx.warp'],text=True).strip().split()[0]
 with (out/(label+'-recovered.log')).open('w') as f:
  subprocess.run([adb,'logcat','-d','--pid='+pid,'-v','threadtime','WiVRn:I','*:S'],stdout=f,check=True)
 print(label,'capture complete',flush=True)
finally:
 for child in reversed(children):
  try:os.killpg(child.pid,signal.SIGTERM)
  except ProcessLookupError:pass
