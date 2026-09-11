from pathlib import Path
import subprocess,time,os,hashlib,json
r=Path('/run/media/nerdrx/Lex/claude');o=r/'nx-scratch/zero-dequant'
while not (r/'nx-scratch/motion-live/zero-base-a-status.json').exists(): time.sleep(1)
subprocess.run(['cmake','--build',str(r/'nx-scratch/nxwarp-atlas-live-build'),'--target','nxvc-vkdec','-j8'],check=True)
results={}
for kind,expected in [('r2','78fbc95b6214590cb07fc821ae484a1a5c98791b0f094b09ef9915b6064e53a6'),('r4','ab25923f09141749fe12f7c5968d73fde60b88672f27f9d326447d9490aacca4')]:
 dst=o/(kind+'.nv12');env=os.environ.copy();env['NXVC_VKD_PLANAR_FLAT']='1'
 p=subprocess.run([str(r/'nx-scratch/nxwarp-atlas-live-build/bin/nxvc-vkdec'),'--in',str(r/f'nx-scratch/r4-palette/candidate-{kind}.nxv'),'--out',str(dst),'--format','ycbcr420','--nv12','--compact-large-centre','--compact-flat64','--independent-tiles','--unorm','0','--stats'],env=env,capture_output=True,text=True,check=True)
 (o/(kind+'.log')).write_text(p.stdout+p.stderr);sha=hashlib.sha256(dst.read_bytes()).hexdigest();results[kind]={'sha256':sha,'expected':expected,'exact':sha==expected};assert sha==expected
(o/'exactness.json').write_text(json.dumps(results,indent=2))
env=os.environ.copy();env['JAVA_HOME']=str(r/'tools/jdk-21.0.12+8');env['ANDROID_HOME']=str(r/'tools/android-sdk');env['PATH']=str(r/'tools/KTX-Software-4.4.0-Linux-x86_64/bin')+':'+env['PATH']
subprocess.run([str(r/'nx-scratch/wt-atlas-live-240/gradlew'),'-p',str(r/'nx-scratch/wt-atlas-live-240'),'assembleRelease','-Pnxwarp_dir='+str(r/'nx-warp'),'-Psuffix=.warp'],env=env,check=True)
(o/'build-ok').write_text('exact fixtures and APK build passed')
