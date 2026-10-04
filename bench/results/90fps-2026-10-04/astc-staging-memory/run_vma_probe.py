#!/usr/bin/env python3
"""Offline Pico VMA mapping/CPU decode-cost probe. Requires connected authorized test device."""
from pathlib import Path
import ctypes, ctypes.util, hashlib, json, os, subprocess, sys
root=Path(__file__).resolve().parent
adb='/run/media/nerdrx/Lex/claude/tools/android-sdk/platform-tools/adb'
remote='/data/local/tmp/nx-astc-vma-probe'
binpath=root/'build/astc-pico'
fixtures={
 'synthetic-all442':(root/'fixtures/all-0x442.astc',root/'fixtures/all-0x442.rgba'),
 'dark-q6':(Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-dualplane-20261004/gpu/validation/dark-final-q6.astc'),Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-dualplane-20261004/gpu/validation/dark-final-q6.rgba')),
 'forest-q6':(Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-dualplane-20261004/gpu/validation/forest-final-q6.astc'),Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-dualplane-20261004/gpu/validation/forest-final-q6.rgba')),
}
def compress(data,codec):
 if codec=='lz4':
  lib=ctypes.CDLL(ctypes.util.find_library('lz4')); bound=lib.LZ4_compressBound; bound.argtypes=[ctypes.c_int]; bound.restype=ctypes.c_int
  fn=lib.LZ4_compress_default; fn.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int]; fn.restype=ctypes.c_int
  dst=ctypes.create_string_buffer(bound(len(data))); src=ctypes.create_string_buffer(data); n=fn(src,dst,len(data),len(dst))
  if n<=0: raise RuntimeError('LZ4 compression failed')
  return dst.raw[:n]
 lib=ctypes.CDLL(ctypes.util.find_library('zstd')); lib.ZSTD_compressBound.argtypes=[ctypes.c_size_t];lib.ZSTD_compressBound.restype=ctypes.c_size_t
 lib.ZSTD_compress.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int];lib.ZSTD_compress.restype=ctypes.c_size_t
 lib.ZSTD_isError.argtypes=[ctypes.c_size_t];lib.ZSTD_isError.restype=ctypes.c_uint
 dst=ctypes.create_string_buffer(lib.ZSTD_compressBound(len(data)));src=ctypes.create_string_buffer(data);n=lib.ZSTD_compress(dst,len(dst),src,len(data),3)
 if lib.ZSTD_isError(n): raise RuntimeError('Zstd compression failed')
 return dst.raw[:n]
def sha(b): return hashlib.sha256(b).hexdigest()
def call(args,**kw): return subprocess.run(args,check=True,**kw)
if len(sys.argv)>1 and sys.argv[1]=='list':
 for n,(a,r) in fixtures.items():print(n,a.stat().st_size,r.stat().st_size)
 raise SystemExit
call([adb,'shell','mkdir','-p',remote])
call([adb,'push',str(binpath),remote+'/astc-pico'],stdout=subprocess.DEVNULL)
records=[]
for name,(astcp,refp) in fixtures.items():
 astc=astcp.read_bytes(); ref=refp.read_bytes(); blocks=astc[16:]
 (root/'inputs').mkdir(exist_ok=True)
 for codec in ('lz4','zstd'):
  payload=compress(blocks,codec)
  for policy in ('current','current-direct','cached'):
   case=root/'results'/name/codec/policy;case.mkdir(parents=True,exist_ok=True)
   for local,remote_name in ((astcp,'input.astc'),(refp,'reference.rgba')):
    call([adb,'push',str(local),remote+'/'+remote_name],stdout=subprocess.DEVNULL)
   localpayload=case/'input.payload';localpayload.write_bytes(payload)
   call([adb,'push',str(localpayload),remote+'/input.payload'],stdout=subprocess.DEVNULL)
   p=subprocess.run([adb,'shell',remote+'/astc-pico',codec,policy],text=True,capture_output=True)
   (case/'stdout.txt').write_text(p.stdout+p.stderr)
   if p.returncode: raise RuntimeError(f'{name}/{codec}/{policy} failed: {p.stdout}{p.stderr}')
   for f in ('result.txt','samples.csv','output.rgba'):
    call([adb,'pull',remote+'/'+f,str(case/f)],stdout=subprocess.DEVNULL)
   out=(case/'output.rgba').read_bytes()
   if len(out)!=len(ref): raise RuntimeError('readback dimensions differ')
   rec={'fixture':name,'codec':codec,'policy':policy,'extent_bytes':len(blocks),'payload_bytes':len(payload),'payload_sha256':sha(payload),'astc_sha256':sha(astc),'reference_sha256':sha(ref),'output_sha256':sha(out),'gpu_rgba_matches_external_exact':out==ref,'result':(case/'result.txt').read_text().strip()}
   (case/'manifest.json').write_text(json.dumps(rec,indent=2)+'\n'); records.append(rec)
   print(name,codec,policy,rec['result'])
(root/'results/manifest.json').write_text(json.dumps(records,indent=2)+'\n')
