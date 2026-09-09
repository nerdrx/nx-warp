"""Build standalone probe against NX Warp's vkmin (host or Android NDK)."""
import argparse,subprocess
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--repo',type=Path,required=True);ap.add_argument('--source',type=Path,default=Path(__file__).parent);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--headers',type=Path);ap.add_argument('--cxx',default='c++');ap.add_argument('--android',action='store_true');a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
common=a.repo/'vk/encoder/tools';cmd=[a.cxx,'-O2','-std=c++20','-I',str(common)]
if a.headers:cmd+=['-I',str(a.headers)]
if a.android:cmd+=['-static-libstdc++']
cmd += [str(a.source/'harness.cpp'),str(common/'vk_min.cpp'),'-lvulkan','-o',str(a.out/'probe')]
subprocess.run(cmd,check=True)
for shader in a.source.glob('sync_probe*.comp'):
 subprocess.run(['glslangValidator','-V','--target-env','vulkan1.1',str(shader),'-o',str(a.out/(shader.stem+'.spv'))],check=True)
