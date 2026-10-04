#!/usr/bin/env python3
"""Compress ASTC block payloads only using installed zstd CLI level 3."""
import csv,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent; results=ROOT/'results'; records=[]
for scene in ('dark','forest','crowd'):
 data=json.loads((results/scene/'metrics/astc-quality.json').read_text())
 for item in data['cases']:
  astc=Path(item['astc_path']); q=int(astc.stem.rsplit('-q',1)[1])
  raw=astc.with_suffix('.blocks.raw'); zst=astc.with_suffix('.blocks.zst3')
  raw.write_bytes(astc.read_bytes()[16:])
  subprocess.run(['zstd','-3','--quiet','--force',str(raw),'-o',str(zst)],check=True)
  decoded=subprocess.run(['zstd','-d','-q','-c',str(zst)],check=True,capture_output=True).stdout
  if decoded!=raw.read_bytes(): raise RuntimeError(f'Zstd3 roundtrip failed: {astc}')
  records.append({'scene':scene,'width':item['width'],'height':item['height'],'quality':q,
    'variant':{2:'current-production-q2-reference',3:'regularized-q3-reference',6:'q6-high-quality-reference',7:'A-edge-gated-endpoint-precision',8:'B-luma-fine-chroma-coarse-endpoints'}[q],
    'astc_sha256':hashlib.sha256(astc.read_bytes()).hexdigest(),
    'lz4_bytes':item['lz4_payload_bytes'],'zstd3_bytes':zst.stat().st_size,
    'rgb_psnr_db':item['rgb_psnr_db'],'rgb_mae':item['rgb_mae'],
    'decoder_exit':item['decoder_return_code']} )
for scene in ('dark','forest','crowd'):
 by={r['quality']:r for r in records if r['scene']==scene}
 q6=by[6]['zstd3_bytes']; q3=by[3]['zstd3_bytes']
 for r in records:
  if r['scene']==scene:
   r['zstd3_savings_vs_q6_pct']=round(100*(1-r['zstd3_bytes']/q6),3)
   r['zstd3_savings_vs_q3_pct']=round(100*(1-r['zstd3_bytes']/q3),3)
with (results/'zstd3-results.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=records[0]);w.writeheader();w.writerows(records)
(results/'zstd3-results.json').write_text(json.dumps({'method':'zstd CLI -3 on ASTC block payload only; excludes standard .astc 16-byte header','records':records},indent=2)+'\n')
