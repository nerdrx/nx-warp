#!/usr/bin/env python3
"""Zstd levels 3/6/9 on existing ASTC blocks; no encoding/GPU work."""
import ctypes as C, ctypes.util, hashlib, json, statistics, time
from pathlib import Path
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'zstd-levels'; OUT.mkdir(exist_ok=True)
SCENES=('dark','forest','crowd')
levels=(3,6,9)

def sha(b): return hashlib.sha256(b).hexdigest()
def bind(lib,name,restype,args):
 f=getattr(lib,name); f.restype=restype; f.argtypes=args; return f
z=C.CDLL(ctypes.util.find_library('zstd'))
create=bind(z,'ZSTD_createCCtx',C.c_void_p,[])
free=bind(z,'ZSTD_freeCCtx',C.c_size_t,[C.c_void_p])
compress=bind(z,'ZSTD_compressCCtx',C.c_size_t,[C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.c_int])
decompress=bind(z,'ZSTD_decompress',C.c_size_t,[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t])
iserr=bind(z,'ZSTD_isError',C.c_uint,[C.c_size_t])
errname=bind(z,'ZSTD_getErrorName',C.c_char_p,[C.c_size_t])
def checked(n):
 if iserr(n): raise RuntimeError(errname(n).decode())
 return int(n)
records=[]
for scene in SCENES:
 for q in (2,3,4):
  astc=ROOT/'results'/scene/f'{scene}-q{q}.astc'
  if q==4 and scene!='crowd' and not astc.exists(): astc=Path('/tmp/nx-astc-quality-test/results')/f'{scene}-q4.astc'
  # The pre-existing q4 render uses the unchanged q4 encoder mode and matching source.
  if not astc.exists():
   if scene=='crowd' and q==4: continue
   raise FileNotFoundError(astc)
  full=astc.read_bytes(); raw=full[16:]
  w,h=(2176,800) if scene=='crowd' else (2176,2176)
  expected=((w+7)//8)*((h+7)//8)*16
  if full[:4]!=bytes((0x13,0xAB,0xA1,0x5C)) or len(raw)!=expected:
   raise RuntimeError(f'invalid ASTC block length {astc}: {len(raw)} != {expected}')
  lz4=(ROOT/'results'/scene/f'{scene}-q6.astc.blocks.lz4').read_bytes()
  q6z3=(ROOT/'results'/scene/f'{scene}-q6.astc.blocks.zst3').read_bytes()
  cctx=create()
  try:
   bound=len(raw)+(len(raw)//128)+1024
   dst=C.create_string_buffer(bound); src=C.create_string_buffer(raw)
   for level in levels:
    samples=[]; packed=b''
    for i in range(10):
     t=time.perf_counter_ns(); n=checked(compress(cctx,dst,bound,src,len(raw),level)); samples.append((time.perf_counter_ns()-t)/1e6); packed=dst.raw[:n]
    outbuf=C.create_string_buffer(len(raw)); source=C.create_string_buffer(packed)
    # Verify exact round trip; decoder supports this one-shot Zstd frame.
    ndec=checked(decompress(outbuf,len(raw),source,len(packed)))
    if ndec!=len(raw) or outbuf.raw!=raw: raise RuntimeError(f'Zstd{level} roundtrip mismatch {astc}')
    zpath=OUT/f'{scene}-q{q}-zstd{level}.blocks.zst'; zpath.write_bytes(packed)
    records.append({'scene':scene,'quality':q,'variant':('current-production-q2' if q==2 else 'regularized-q3' if q==3 else 'pre-existing-q4-unchanged-mode'),'astc_path':str(astc),'astc_sha256':sha(full),'source_sha256':json.loads((ROOT/'results'/scene/'metrics/astc-quality.json').read_text())['cases'][0]['source_sha256'],'raw_astc_blocks_bytes':len(raw),'q6_lz4_bytes':len(lz4),'q6_zstd3_bytes':len(q6z3),'zstd_level':level,'zstd_bytes':len(packed),'zstd_sha256':sha(packed),'compress_ms_median_10_reused_CCtx_calls':round(statistics.median(samples),4),'compress_samples_ms':[round(x,4) for x in samples],'decompress_exact':True,'saved_vs_q6_lz4_pct':round(100*(1-len(packed)/len(lz4)),3),'saved_vs_q6_zstd3_pct':round(100*(1-len(packed)/len(q6z3)),3),'reaches_50pct_vs_q6_lz4':len(packed)<=len(lz4)*0.5})
  finally: free(cctx)
(OUT/'levels3-6-9.json').write_text(json.dumps({'method':'ZSTD_compressCCtx with one reused CCtx per ASTC case, 10 host calls per level; exact ZSTD_decompress roundtrip; ASTC 16-byte header excluded','no_gpu_or_encoding_reruns':True,'records':records},indent=2)+'\n')
print(json.dumps(records,indent=2))
