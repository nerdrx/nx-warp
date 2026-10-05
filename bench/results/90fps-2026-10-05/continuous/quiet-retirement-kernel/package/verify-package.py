#!/usr/bin/env python3
"""Review configured unsigned Android package; never install or enable it."""
import argparse, hashlib, json, os, subprocess, zipfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('repo',type=Path);p.add_argument('build_tools',type=Path);p.add_argument('output',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
apk=a.repo/'build/outputs/apk/release/wt-pyrowave-probe-release-unsigned.apk'
stripped=a.repo/'build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a/libwivrn.so'
native=a.repo/'build/intermediates/cxx/RelWithDebInfo/33s4w1c5/obj/arm64-v8a/libwivrn.so'
head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=a.repo,text=True).strip()
with zipfile.ZipFile(apk) as z:data=z.read('lib/arm64-v8a/libwivrn.so')
assert data==stripped.read_bytes()
assert head.encode() in data
assert all(token in data for token in (b'debug.wivrn.nx.recovery_poll',b'debug.wivrn.nx.astc_deadline'))
exits={}
for name,command in [('badging',['aapt','dump','badging']),('alignment',['zipalign','-c','-v','4']),('signature',['apksigner','verify'])]:
 result=subprocess.run([str(a.build_tools/command[0]),*command[1:],str(apk)],capture_output=True,text=True)
 exits[name]=result.returncode;(a.output/(name+'.log')).write_text(result.stdout+result.stderr)
assert exits=={'badging':0,'alignment':0,'signature':1},exits
assert 'ERROR: Missing META-INF/MANIFEST.MF' in (a.output/'signature.log').read_text()
# Extract to scratch output solely for readelf review; do not publish this binary.
copy=a.output/'packaged-libwivrn.so';copy.write_bytes(data)
ids=[]
for label,file in [('packaged',copy),('unstripped',native)]:
 notes=subprocess.check_output(['readelf','-n',str(file)],text=True);(a.output/(label+'-notes.txt')).write_text(notes)
 ids.append(next(line.split(':',1)[1].strip() for line in notes.splitlines() if 'Build ID:' in line))
assert ids[0]==ids[1]
result={'source_commit':head,'apk':str(apk),'apk_sha256':hashlib.sha256(apk.read_bytes()).hexdigest(),'packaged_native_sha256':hashlib.sha256(data).hexdigest(),'unstripped_native_sha256':hashlib.sha256(native.read_bytes()).hexdigest(),'packaged_matches_gradle_stripped':True,'native_build_id':ids[0],'build_id_matches_unstripped':True,'committed_source_identity_present':True,'both_option_property_strings_present':True,'tool_exits':exits,'manifest_badging_first_line':(a.output/'badging.log').read_text().splitlines()[0],'scope':'unsigned release package; no signing/install/activation/runtime execution'}
(a.output/'review.json').write_text(json.dumps(result,indent=2)+'\n')
print('Unsigned package identity/bytes/build ID/alignment checks pass; expected signature failure.')
