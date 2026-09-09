import subprocess,signal,json
from pathlib import Path
p=Path(__file__).parent;a='/home/nerdrx/.local/bin/adb'
subprocess.run([a,'shell','am','force-stop','org.meumeu.wivrn.nx.warp'],check=True)
with (p/'streamed-client.log').open('w') as f:
 log=subprocess.Popen([a,'logcat','-T','1','-v','threadtime','WiVRn:I','PxrMetric:I','*:S'],stdout=f)
 try:r=subprocess.run(['python3','nx-scratch/motion-live/capture_live.py','handoff-streamed','1','4000','60','0'])
 finally:log.terminate();log.wait(timeout=5)
status=json.loads(Path('nx-scratch/motion-live/handoff-streamed-status.json').read_text());pid=status['pid']
lines=(p/'streamed-client.log').read_text().splitlines();lines=[s for s in lines if len(s.split())>=4 and s.split()[2]==pid]
(p/'client.log').write_text('\n'.join(lines)+'\n');print(status)
assert status['complete'] and status['client_alive']
