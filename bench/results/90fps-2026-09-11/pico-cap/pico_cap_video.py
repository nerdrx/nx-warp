from pathlib import Path
import subprocess,time,json,os
r=Path('/run/media/nerdrx/Lex/claude');live=r/'nx-scratch/motion-live';out=r/'nx-scratch/motion-regions/pico-cap';out.mkdir(exist_ok=True);adb='/home/nerdrx/.local/bin/adb'
base=r/'nx-scratch/live/xdg/wivrn/config.planar-gpu-graduated-copybaseline90.json';props=['motion_mode','motion_cap','fdm','ready_wait_us','peripheral_smooth','capture'];saved={k:subprocess.check_output([adb,'shell','getprop','debug.wivrn.nx.'+k],text=True).strip() for k in props};(out/'saved-properties.json').write_text(json.dumps(saved,indent=2))
def restart(config,*flags):subprocess.run(['python3',str(live/'restart_server.py'),(live/'server-current.pid').read_text().strip(),'--force-owned','--config',str(config),*flags],check=True)
try:
 restart(r/'nx-scratch/hw-codec-probe/vaapi-hevc-2688-10bit.json','--always-motion-field','--source60','--motion-clock')
 for k,v in [('motion_mode','headset'),('motion_cap','1')]:subprocess.run([adb,'shell','setprop','debug.wivrn.nx.'+k,v],check=True)
 p=subprocess.Popen(['python3',str(live/'capture_live.py'),'pico-cap11-video','3','1000','20','0','3'])
 time.sleep(8)
 subprocess.run([adb,'shell','screenrecord','--time-limit','8','--size','1920x960','/sdcard/nx-cap-test.mp4'],check=True,timeout=20)
 subprocess.run([adb,'pull','/sdcard/nx-cap-test.mp4',str(out/'pico-video.mp4')],check=True)
 subprocess.run([adb,'shell','rm','/sdcard/nx-cap-test.mp4'],check=True)
 with (out/'screen.png').open('wb') as f:subprocess.run([adb,'exec-out','screencap','-p'],stdout=f,check=True)
 p.wait(timeout=40)
finally:
 for k,v in saved.items():subprocess.run([adb,'shell','setprop','debug.wivrn.nx.'+k,v if v else "''"],check=True)
 restart(base,'--always-motion-field-off','--source60-off','--motion-clock-off')
 subprocess.run([adb,'shell','am','start','-a','android.intent.action.VIEW','-d','wivrn://192.168.1.2','org.meumeu.wivrn.nx.warp'],check=True)
