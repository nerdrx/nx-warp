#!/usr/bin/env python3
from pathlib import Path
import csv,json,statistics
import matplotlib.pyplot as plt
out=Path(__file__).resolve().parent
rows=list(csv.DictReader((out/'runs.csv').open()))
for r in rows:
 r['wire_bytes']=int(r['wire_bytes']);r['encode_ms']=float(r['encode_ms'])
cache=[r for r in rows if r['kind']=='cache']
cp=[r for r in rows if r['kind']=='copy']
pairs=[]
for scene in ('forest','dark'):
 for shift in ('0','8'):
  b=next(r for r in rows if r['kind']=='predictor' and r['scene']==scene and r['shift']==shift and r['mode']=='baseline')
  p=next(r for r in rows if r['kind']=='predictor' and r['scene']==scene and r['shift']==shift and r['mode']=='predictor')
  pairs.append({'scene':scene,'shift':shift,'baseline_wire_bytes':b['wire_bytes'],'predictor_wire_bytes':p['wire_bytes'],'wire_saving_pct':100*(b['wire_bytes']-p['wire_bytes'])/b['wire_bytes'],'baseline_encode_ms':b['encode_ms'],'predictor_encode_ms':p['encode_ms'],'encode_delta_ms':p['encode_ms']-b['encode_ms']})
pico=list(csv.DictReader((out/'pico_decode.csv').open()))
summary={'scope':'sanitized provisional benchmark','predictor_pairs':pairs,'raw_exact_hashes_verified':4,'details':'full-frame NXDF codec payload including detail+safety; excludes transport/FEC/padding','pico_decoder':'production-zstd-bench-pico.csv: pointer-local-v2 versus ordinary-zstd-v1; exact roundtrip field retained; NEON excluded','fixture_scope':'CPU NV12 conversion, two identical photo eyes; not real game or physical motion'}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
fig,axs=plt.subplots(1,3,figsize=(14,4.3),constrained_layout=True)
labels=[f"{x['scene']} s{x['shift']}" for x in pairs];x=list(range(4));w=.36
axs[0].bar([i-w/2 for i in x],[r['baseline_wire_bytes']/1000 for r in pairs],w,label='baseline');axs[0].bar([i+w/2 for i in x],[r['predictor_wire_bytes']/1000 for r in pairs],w,label='predictor');axs[0].set_xticks(x,labels);axs[0].set_ylabel('full-frame codec payload kB');axs[0].set_title('Payload: detail+safety');axs[0].legend(fontsize=8)
for i,scene in enumerate(('forest','dark')):
 off=[float(r['encode_ms']) for r in cache if r['scene']==scene and r['mode']=='cache-off'];on=[float(r['encode_ms']) for r in cache if r['scene']==scene and r['mode']=='cache-on'];axs[1].bar(i-w/2,statistics.mean(off),w,color='#377eb8',label='cache off' if i==0 else None);axs[1].bar(i+w/2,statistics.mean(on),w,color='#e41a1c',label='cache on' if i==0 else None)
axs[1].set_xticks([0,1],['forest','dark']);axs[1].set_ylabel('encode median ms');axs[1].set_title('Reusable cache');axs[1].legend(fontsize=8)
cm=[]
for mode in ('copy-1-cache-0','copy-0-cache-0','copy-0-cache-1','copy-1-cache-1'):
 v=[float(r['encode_ms']) for r in cp if r['mode']==mode]
 if v:cm.append((mode,statistics.mean(v)))
axs[2].bar(range(len(cm)),[v for _,v in cm],color=['#55a868' if m.startswith('copy-1') else '#c44e52' for m,_ in cm]);axs[2].set_xticks(range(len(cm)),[m.replace('copy-','c').replace('-cache-','/cache') for m,_ in cm],rotation=25,ha='right');axs[2].set_ylabel('encode median ms');axs[2].set_title('Copy-bypass check')
fig.suptitle('NXDF predictor/cache provisional results');fig.savefig(out/'comparison.png',dpi=160);fig.savefig(out/'comparison.svg');plt.close(fig)
