#!/usr/bin/env python3
"""Build scratch fixture using an already configured production server cache."""
import argparse, pathlib, subprocess, shlex, shutil, hashlib, json
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('repo',type=pathlib.Path);p.add_argument('output',type=pathlib.Path)
p.add_argument('--build',type=pathlib.Path)
a=p.parse_args();src=a.repo.resolve();out=a.output.resolve();build=(a.build or src/'build-server').resolve();here=pathlib.Path(__file__).resolve().parent
assert out != here, 'Use a separate scratch output directory'
out.mkdir(parents=True,exist_ok=True)
for n in ['fixture.cpp','runtime-check.h','sample-output.comp']:shutil.copy2(here/n,out/n)
(out/'empty-config.json').write_text('{}\n')
commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','wivrn-server'],text=True).splitlines()
c=shlex.split(next(x for x in commands if 'layer_squasher.cpp.o -c ' in x))
c=[x for x in c if not x.startswith(('-fdeps-','-fmodule-mapper=')) and x!='-fmodules-ts']
original='server/CMakeFiles/wivrn-server.dir/compositor/layer_squasher.cpp.o'
c=[x.replace(original,str(out/'fixture.o')) for x in c]
c[c.index('-c')+1]=str(out/'fixture.cpp')
l=shlex.split(next(x for x in commands if ' -o server/wivrn-server ' in x))
l=[x for x in l if x not in {':','&&','server/CMakeFiles/wivrn-server.dir/main.cpp.o'}]
l[l.index('-o')+1]=str(out/'squasher-fixture');l.insert(l.index('-o'),str(out/'fixture.o'))
l=[('-Wl,--dependency-file='+str(out/'link.d')) if x.startswith('-Wl,--dependency-file=') else x for x in l]
for name,args in [('shader',['glslangValidator','-V','--target-env','vulkan1.3',str(out/'sample-output.comp'),'-o',str(out/'sample-output.spv')]),('spirv-val',['spirv-val','--target-env','vulkan1.3',str(out/'sample-output.spv')]),('compile',c),('link',l)]:
 (out/(name+'.command')).write_text(shlex.join(args)+'\n')
 with (out/(name+'.log')).open('w') as log:r=subprocess.run(args,cwd=build,stdout=log,stderr=subprocess.STDOUT,timeout=240)
 print(name,r.returncode,flush=True)
 if r.returncode:raise SystemExit(r.returncode)
# All cached object/library bytes actually named in the linker command.
inputs={}
for x in l:
 f=pathlib.Path(x);f=f if f.is_absolute() else build/f
 if f.is_file() and f.suffix in {'.o','.a','.so'}:inputs[str(f)]=hashlib.sha256(f.read_bytes()).hexdigest()
source_files=['server/compositor/layer_squasher.cpp','server/compositor/layer_squasher.h','server/utils/wivrn_vk_bundle.cpp','server/utils/wivrn_vk_bundle.h','server/driver/wivrn_hmd.cpp','server/driver/configuration.cpp']
result={'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=src,text=True).strip(),'source_sha256':{f:hashlib.sha256((src/f).read_bytes()).hexdigest() for f in source_files},'linked_inputs_sha256':inputs,'binary_sha256':hashlib.sha256((out/'squasher-fixture').read_bytes()).hexdigest(),'scope':'production cached objects linked with scratch entrypoint; no production main/session execution; not full layer_commit'}
(out/'provenance.json').write_text(json.dumps(result,indent=2)+'\n')
