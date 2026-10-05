#!/usr/bin/env python3
"""Short authorized CPU fixture on an asleep, idle headset; never launch VR."""
import argparse, datetime, hashlib, json, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('serial');p.add_argument('elf',type=Path);p.add_argument('output',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);remote='/data/local/tmp/nx_quiet_gate'
def adb(*args):return subprocess.run(['adb','-s',a.serial,*args],capture_output=True,text=True,timeout=10)
def power_state():
 r=adb('shell','dumpsys','power');assert r.returncode==0
 text='\n'.join(x for x in r.stdout.splitlines() if 'mWakefulness=' in x or 'Display Power:' in x)+'\n'
 assert 'mWakefulness=Asleep' in text and 'state=OFF' in text
 return text
before=power_state();(a.output/'immediate-preflight.txt').write_text(before)
for process in ['org.meumeu.wivrn.nx.local','nx_quiet_gate']:
 r=adb('shell','pidof',process);assert r.returncode==1 and not r.stdout.strip()
r=adb('shell','command','-v','timeout');assert r.returncode==0 and r.stdout.strip()
keys=['debug.wivrn.nx.recovery_poll','debug.wivrn.nx.astc_deadline','debug.wivrn.nx.astc_queue_timing']
def properties():
 values={}
 for key in keys:
  r=adb('shell','getprop',key);assert r.returncode==0;values[key]=r.stdout.strip()
 return values
props=properties();(a.output/'existing-properties.json').write_text(json.dumps(props,indent=2)+'\n')
try:
 r=adb('push',str(a.elf),remote);(a.output/'push.log').write_text(r.stdout+r.stderr);assert r.returncode==0
 assert adb('shell','chmod','700',remote).returncode==0
 expected=hashlib.sha256(a.elf.read_bytes()).hexdigest();r=adb('shell','sha256sum',remote)
 assert r.returncode==0 and r.stdout.split()[0]==expected;(a.output/'remote-hash.txt').write_text(r.stdout)
 started=datetime.datetime.now(datetime.timezone.utc).isoformat()
 r=adb('shell','timeout','5',remote);(a.output/'pico-normal.log').write_text(r.stdout+r.stderr)
 (a.output/'run-result.json').write_text(json.dumps({'utc_start':started,'command':['timeout','5',remote],'exit':r.returncode,'elf_sha256':expected,'scope':'standalone CPU socket fixture, not viewer/JNI/properties/decoder/display'},indent=2)+'\n')
 assert r.returncode==0 and 'checks=96 failures=0' in r.stdout and 'checks=72 failures=0' in r.stdout
finally:
 r=adb('shell','rm','-f',remote);(a.output/'cleanup.log').write_text('exit='+str(r.returncode)+'\n'+r.stdout+r.stderr);assert r.returncode==0
 (a.output/'after-power.txt').write_text(power_state())
 after=properties();assert props==after;(a.output/'after-properties.json').write_text(json.dumps(after,indent=2)+'\n')
 r=adb('shell','pidof','nx_quiet_gate');assert r.returncode==1 and not r.stdout.strip()
print('168 functional checks pass; device asleep, properties unchanged, executable removed.')
