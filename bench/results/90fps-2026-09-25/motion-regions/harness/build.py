#!/usr/bin/env python3
"""Build isolated direct-codec A/B test binaries using an existing CMake build."""
import argparse, json, shlex, subprocess
from pathlib import Path

ap=argparse.ArgumentParser()
ap.add_argument('--baseline',type=Path,required=True,help='checkout at baseline commit 070b671')
ap.add_argument('--candidate',type=Path,required=True,help='candidate repository checkout')
ap.add_argument('--build',type=Path,required=True,help='configured host build with compile_commands.json')
ap.add_argument('--out',type=Path,default=Path('build-out'))
ap.add_argument('--test-dir',type=Path,default=Path(__file__).resolve().parent)
a=ap.parse_args()
build=a.build.resolve(); out=a.out.resolve(); test_dir=a.test_dir.resolve()
rows=json.loads((build/'compile_commands.json').read_text())
row=next(r for r in rows if r['file'].endswith('/server/encoder/nxwarp_codec_direct.cpp'))
cwd=Path(row['directory']).resolve(); common_args=list(row.get('arguments') or shlex.split(row['command']))
link=shlex.split((build/'server/CMakeFiles/wivrn-server.dir/link.txt').read_text())
link=[x for x in link if not x.endswith('/main.cpp.o') and not x.endswith('/nxwarp_codec_direct.cpp.o') and '--dependency-file=' not in x and x!='-DNDEBUG']
outidx=link.index('-o')

def clean(args):
    result=[]; skip=False; dep={'-MF','-MT','-MQ','-MJ'}
    for x in args:
        if skip: skip=False; continue
        if x in {'-DNDEBUG','-MD','-MMD','-MP'}: continue
        if x in dep: skip=True; continue
        if any(x.startswith(flag) for flag in dep): continue
        result.append(x)
    return result

for name,repo,regional in (('baseline',a.baseline.resolve(),False),('regions',a.candidate.resolve(),True)):
    dest=out/name; dest.mkdir(parents=True,exist_ok=True)
    args=clean(common_args.copy())
    args[next(i for i,x in enumerate(args) if x.endswith('/server/encoder/nxwarp_codec_direct.cpp'))]=str(repo/'server/encoder/nxwarp_codec_direct.cpp')
    args[1:1]=[f'-I{repo/"common"}',f'-I{repo/"server/encoder"}',f'-I{repo}']
    obj=dest/'codec.o'; args[args.index('-o')+1]=str(obj)
    subprocess.run(args,cwd=cwd,check=True)
    test=clean(common_args.copy())
    test=[x for x in test if not x.endswith('/server/encoder/nxwarp_codec_direct.cpp') and not x.endswith('.cpp')]
    test[1:1]=(['-DNX_DIRECT_TEST_WIDTH=2176'] + (['-DNX_DIRECT_TEST_REGIONS'] if regional else []) +
               [f'-I{repo/"common"}',f'-I{repo/"server/encoder"}',f'-I{repo}',f'-I{test_dir}'])
    test.insert(1,str(test_dir/'motion_regions_gpu_test_wrapper.cpp'))
    testobj=dest/'test.o'; test[test.index('-o')+1]=str(testobj)
    subprocess.run(test,cwd=cwd,check=True)
    cmd=link.copy(); cmd[outidx+1]=str(dest/'bench')
    cmd.insert(outidx+2,str(obj)); cmd.insert(outidx+2,str(testobj))
    subprocess.run(cmd,cwd=build/'server',check=True)
    print(dest/'bench')
