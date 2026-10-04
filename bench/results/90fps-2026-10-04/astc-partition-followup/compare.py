#!/usr/bin/env python3
"""Recompute guarded q6 partition-vs-dualplane results; writes aggregate CSV only."""
from pathlib import Path
import ctypes, ctypes.util, csv, hashlib, math, subprocess, tempfile
R=Path('/run/media/nerdrx/Lex/claude/nx-scratch'); OUT=Path(__file__).parent
DEC=R/'nx-xuastc-20261003/decode_astc'
CASES={
 'dark':(R/'astc-native-colour-20261004/crops/dark-native-1920x1080.rgba',R/'astc-dualplane-20261004/gpu/validation/dark-baseline-canonical-q6.astc',R/'astc-dualplane-20261004/gpu/validation/quality-policy-20261004/dark-q6-candidate.astc.blocks.lz4',R/'astc-partition-queue-20261004/results/exact-user-final/dark-queue.astc',R/'astc-partition-queue-20261004/results/exact-user-final/dark-queue-decoded.rgba'),
 'forest':(R/'astc-native-colour-20261004/crops/forest-native-1920x1080.rgba',R/'astc-dualplane-20261004/gpu/validation/forest-baseline-canonical-q6.astc',R/'astc-dualplane-20261004/gpu/validation/quality-policy-20261004/forest-q6-candidate.astc.blocks.lz4',R/'astc-partition-queue-20261004/results/exact-user-final/forest-queue.astc',R/'astc-partition-queue-20261004/results/exact-user-final/forest-queue-decoded.rgba')}
L=ctypes.CDLL(ctypes.util.find_library('lz4'));L.LZ4_decompress_safe.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int];L.LZ4_decompress_safe.restype=ctypes.c_int
Z=ctypes.CDLL(ctypes.util.find_library('zstd'));Z.ZSTD_compressBound.argtypes=[ctypes.c_size_t];Z.ZSTD_compressBound.restype=ctypes.c_size_t;Z.ZSTD_compress.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int];Z.ZSTD_compress.restype=ctypes.c_size_t;Z.ZSTD_isError.argtypes=[ctypes.c_size_t]
def sha(b):return hashlib.sha256(b).hexdigest()
def zstd3(b):
 o=ctypes.create_string_buffer(Z.ZSTD_compressBound(len(b)));i=ctypes.create_string_buffer(b);n=Z.ZSTD_compress(o,len(o),i,len(b),3)
 if Z.ZSTD_isError(n):raise RuntimeError('Zstd failure')
 return o.raw[:n]
def score(src,out,mask):
 err=n=0
 for i in range(0,len(src),4):
  if mask is not None and not mask[i//4]:continue
  for c in range(3):d=src[i+c]-out[i+c];err+=d*d;n+=1
 mse=err/n;return 10*math.log10(255*255/mse),mse
def masks(src,w=1920,h=1080):
 hc=bytearray(w*h);ce=bytearray(w*h)
 for y in range(h):
  for x in range(w):
   i=(y*w+x)*4;r,g,b=src[i:i+3];span=max(r,g,b)-min(r,g,b)
   if span>=40:hc[y*w+x]=1
   if span>=24 and (x+1<w or y+1<h):
    j=i+4 if x+1<w else i+4*w
    if max(abs(src[i+c]-src[j+c]) for c in range(3))>=20:ce[y*w+x]=1
 return hc,ce
def decode_guarded(header,lz4path,tmp,scene):
 z=lz4path.read_bytes();raw=ctypes.create_string_buffer(518400);src=ctypes.create_string_buffer(z);n=L.LZ4_decompress_safe(src,raw,len(z),518400)
 if n!=518400:raise RuntimeError(f'{scene}: invalid LZ4 block payload size {n}')
 ast=tmp/f'{scene}-guarded-q6.astc';rgba=tmp/f'{scene}-guarded-q6.rgba';ast.write_bytes(header+raw.raw[:n])
 subprocess.run([str(DEC),str(ast),str(rgba)],check=True,capture_output=True)
 return raw.raw[:n],ast.read_bytes(),rgba.read_bytes()
fields=['scene','source_sha256','guarded_policy_spv_sha256','guarded_blocks_lz4_sha256','guarded_astc_sha256','guarded_rgba_sha256','partition_astc_sha256','partition_rgba_sha256','raw_astc_bytes_each','guarded_zstd3_bytes','partition_zstd3_bytes','zstd3_delta_bytes','guarded_mode442_blocks','partition_mode053_blocks','high_chroma_pixels','chroma_edge_pixels']
for region in ('full','high_chroma_pixels','chroma_edges'):
 for variant in ('guarded_dualplane','partition'):fields += [f'{region}_{variant}_psnr_db',f'{region}_{variant}_mse']
 fields += [f'{region}_partition_minus_guarded_db']
rows=[]
for scene,(sp,baseline,lz4p,pa,pr) in CASES.items():
 source=sp.read_bytes();baseline_bytes=baseline.read_bytes();assert len(source)==8294400 and len(baseline_bytes)==518416
 with tempfile.TemporaryDirectory(prefix='astc-q6-verify-') as td:
  guarded_blocks,guarded_astc,guarded_rgba=decode_guarded(baseline_bytes[:16],lz4p,Path(td),scene)
 part_astc=pa.read_bytes();part_rgba=pr.read_bytes();assert len(source)==len(guarded_rgba)==len(part_rgba)==8294400
 hc,ce=masks(source)
 row={'scene':scene,'source_sha256':sha(source),'guarded_policy_spv_sha256':'b8447734bf2ac2e5618522a8c923d3f37c96d4dd401a30f609bceaf1647ec4d2','guarded_blocks_lz4_sha256':sha(lz4p.read_bytes()),'guarded_astc_sha256':sha(guarded_astc),'guarded_rgba_sha256':sha(guarded_rgba),'partition_astc_sha256':sha(part_astc),'partition_rgba_sha256':sha(part_rgba),'raw_astc_bytes_each':len(guarded_astc),'guarded_zstd3_bytes':len(zstd3(guarded_astc)),'partition_zstd3_bytes':len(zstd3(part_astc)),'zstd3_delta_bytes':len(zstd3(part_astc))-len(zstd3(guarded_astc)),'guarded_mode442_blocks':sum((int.from_bytes(guarded_blocks[i:i+2],'little')&0x7ff)==0x442 for i in range(0,len(guarded_blocks),16)),'partition_mode053_blocks':sum((int.from_bytes(part_astc[i:i+2],'little')&0x7ff)==0x053 for i in range(16,len(part_astc),16)),'high_chroma_pixels':sum(hc),'chroma_edge_pixels':sum(ce)}
 for name,mask in [('full',None),('high_chroma_pixels',hc),('chroma_edges',ce)]:
  a=score(source,guarded_rgba,mask);b=score(source,part_rgba,mask)
  row[f'{name}_guarded_dualplane_psnr_db']=round(a[0],5);row[f'{name}_guarded_dualplane_mse']=round(a[1],5);row[f'{name}_partition_psnr_db']=round(b[0],5);row[f'{name}_partition_mse']=round(b[1],5);row[f'{name}_partition_minus_guarded_db']=round(b[0]-a[0],5)
 rows.append(row)
with (OUT/'comparison.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
