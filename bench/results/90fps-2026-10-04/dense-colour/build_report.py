#!/usr/bin/env python3
"""Regenerate CPU metrics and the cohort-separated quality/payload chart."""
import csv, json, math
from pathlib import Path
import numpy as np
import matplotlib.pyplot as plt

ROOT=Path(__file__).resolve().parent
part=ROOT/'cpu-partition'; chroma=ROOT/'chroma-probe/source-chroma.csv'
source_path=Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-partitions-20261004/fixtures/crowd-centre-512.png')
upstream=Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-partitions-20261004/results')
src=np.asarray(__import__('PIL').Image.open(source_path).convert('RGB'),dtype=np.uint8)
# The supplied CPU comparison panel contains x=0..512, y=12..512 from the centre crop.
source_extent={'kind':'entire 512x512 centre crop','xyxy':[0,0,512,512],'pixels':[512,512]}
panel_extent={'kind':'displayed focus crop','xyxy':[0,12,512,512],'pixels':[512,500]}
variants=[('one-partition baseline','baseline.rgba','baseline.zst'),('two-partition candidate','candidate-packed.rgba','candidate.zst'),('5% tile-gated fallback','selected.rgba','selected.zst')]
rows=[]
for name,rgba,zst in variants:
    decoded=np.fromfile(upstream/rgba,dtype=np.uint8).reshape(512,512,4)[:,:,:3]
    encoded_size=(upstream/zst).stat().st_size
    record={'variant':name,'zstd3_block_bytes':encoded_size}
    for label,box in [('full',source_extent['xyxy']),('display_crop',panel_extent['xyxy'])]:
        x0,y0,x1,y1=box; d=src[y0:y1,x0:x1].astype(np.float64)-decoded[y0:y1,x0:x1].astype(np.float64)
        mse=float(np.mean(d*d)); record[f'{label}_psnr_db']=10*math.log10(255.0**2/mse); record[f'{label}_mae']=float(np.abs(d).mean())
    rows.append(record)
with (part/'cpu-two-partition-metrics.csv').open('w',newline='') as f:
    w=csv.writer(f);w.writerow(['variant','zstd3_block_bytes','full_source_psnr_db','full_source_mae','display_crop_xyxy','display_crop_psnr_db','display_crop_mae'])
    for r in rows:w.writerow([r['variant'],r['zstd3_block_bytes'],f"{r['full_psnr_db']:.6f}",f"{r['full_mae']:.6f}",'0,12,512,512',f"{r['display_crop_psnr_db']:.6f}",f"{r['display_crop_mae']:.6f}"])
(part/'cpu-two-partition-metrics.json').write_text(json.dumps({'method':'CPU two-partition probe vs the 512x512 crowd-centre reference; adaptive fallback chooses candidate 8x8 tiles only when squared RGB error improves by at least 5%. Metrics independently recomputed from the supplied decoded outputs.','source':{'name':source_path.name,'width':512,'height':512,'full_extent':source_extent,'focus_crop':panel_extent},'records':rows},indent=2)+'\n')
# Three separate cohorts. Never pool points across different crop or transform definitions.
dense=json.loads((ROOT/'dense-colour/fair-results.json').read_text())['records']
with chroma.open(newline='') as f: cr=list(csv.DictReader(f))
cr=[r for r in cr if r['scene']=='crowd' and r['quality']=='6']
native_path=Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-projected-line-20261004/results/quality.json')
native=json.loads(native_path.read_text())
chromamap={'original_rgb':'Original RGB','nearest':'2x2 chroma · nearest','bilinear':'2x2 chroma · bilinear'}
fig,axs=plt.subplots(1,3,figsize=(15,5.2),layout='constrained')
# Matched fit: full 2176x800 preprocessed crowd reference.
ax=axs[0]
for bs,color,marker in [('8x8','#1769aa','o'),('6x6','#d1495b','s')]:
 for q,fill in [(6,1.0),(2,0.55)]:
  rs=[r for r in dense if r['scene']=='crowd' and r['block']==bs and r['quality']==q]
  for r in rs:ax.scatter(r['zstd3_bytes'],r['psnr_db'],s=85,marker=marker,color=color,alpha=fill,label=f'{bs} q{q}')
  for r in rs:
   offset=(-6,5) if r['block']=='6x6' and r['quality']==6 else (4,5)
   ha='right' if offset[0]<0 else 'left'
   ax.annotate(f"{bs} q{q}",(r['zstd3_bytes'],r['psnr_db']),xytext=offset,textcoords='offset points',ha=ha,fontsize=8)
ax.set_title('Matched block fit\n2176×800; origin unknown')
# Chroma source reconstruction: same fixture, with q6 ASTC output.
ax=axs[1]
for r in cr:
 x=int(r['zstd3_block_bytes']); y=float(r['decoded_vs_original_psnr_db'])
 ax.scatter(x,y,s=85,label=chromamap[r['variant']])
 # Nearby original/nearest points are easier to distinguish in a compact legend.
ax.legend(loc='center left',bbox_to_anchor=(.53,.57),fontsize=8,framealpha=.95)
ax.set_title('Chroma probe · q6\n2176×800; origin unknown')
# Independent 512x512 centre crop partition data.
ax=axs[2]
for r in rows:
 ax.scatter(r['zstd3_block_bytes'],r['full_psnr_db'],s=85)
 dx=-8 if r['variant']=='5% tile-gated fallback' else 8
 ha='right' if dx<0 else 'left'
 ax.annotate(r['variant'],(r['zstd3_block_bytes'],r['full_psnr_db']),xytext=(dx,5),textcoords='offset points',ha=ha,fontsize=8)
ax.set_title('CPU partition probe · q6\n512×512 centre crop')
from matplotlib.ticker import FuncFormatter
for ax,step in zip(axs,(50000,20000,1000)):
 ax.xaxis.set_major_locator(plt.MultipleLocator(step))
 ax.xaxis.set_major_formatter(FuncFormatter(lambda x,pos:f'{x/1000:.0f}k'))
 ax.set_xlabel('Zstd level 3 block payload (bytes)');ax.set_ylabel('Decoded RGB PSNR vs reference (dB)');ax.grid(alpha=.22)
fig.suptitle('ASTC quality–payload probes · separate cohorts, no pooled comparison',fontsize=14)
fig.savefig(ROOT/'figures/quality-vs-zstd3.png',dpi=180);plt.close(fig)
# Add a compact plot-data table so figure points are auditable.
with (ROOT/'figures/quality-vs-zstd3.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['cohort','source_extent','variant','quality','zstd3_bytes','psnr_db'])
 for r in dense:
  if r['scene']=='crowd':w.writerow(['matched-block-fit','2176x800 preprocessed crowd','ASTC '+r['block'],r['quality'],r['zstd3_bytes'],r['psnr_db']])
 for r in cr:w.writerow(['chroma-probe','2176x800 preprocessed crowd',chromamap[r['variant']],r['quality'],r['zstd3_block_bytes'],r['decoded_vs_original_psnr_db']])
 for r in rows:w.writerow(['cpu-two-partition','512x512 centre crop',r['variant'],6,r['zstd3_block_bytes'],r['full_psnr_db']])
 for r in native:w.writerow(['exact-native-photo',f"{r['scene']} 1920x1080 full photo",r['mode'],r['q'],r['zstd3_bytes'],r['psnr']])
# Exact-source 8x8 projected-line quality points, kept separate by photo and q.
fig,axs=plt.subplots(1,2,figsize=(9.5,4.4),layout='constrained')
for ax,scene in zip(axs,('dark','forest')):
 rr=[r for r in native if r['scene']==scene]
 for mode,color,marker in [('base','#1769aa','o'),('projected','#d1495b','s')]:
  points=sorted((r for r in rr if r['mode']==mode),key=lambda r:r.get('q',6))
  ax.plot([r['zstd3_bytes']/1000 for r in points],[r['psnr'] for r in points],color=color,alpha=.7)
  for r in points:
   q=r.get('q',6); ax.scatter(r['zstd3_bytes']/1000,r['psnr'],s=62,marker=marker,color=color)
   dy=(7 if mode=='projected' else -10) if (scene=='forest' and q in (2,4)) else 4
   dx=5 if mode=='projected' else -17
   ax.annotate(f"q{q}",(r['zstd3_bytes']/1000,r['psnr']),xytext=(dx,dy),textcoords='offset points',fontsize=8)
 ax.set_title(f"{scene.title()} · exact 1920×1080 photo")
 ax.set_xlabel('Zstd level 3 ASTC payload (kB)');ax.set_ylabel('Decoded RGB PSNR (dB)');ax.grid(alpha=.22)
from matplotlib.lines import Line2D
axs[0].legend(handles=[Line2D([0],[0],color='#1769aa',marker='o',label='current fit'),Line2D([0],[0],color='#d1495b',marker='s',label='projected line')],fontsize=8)
fig.suptitle('Same 8×8 ASTC mode · quality versus payload by exact source',fontsize=12)
fig.savefig(ROOT/'figures/native-photo-quality-vs-zstd3.png',dpi=180);plt.close(fig)
# q6 endpoint-refit rejection: same exact photos, fit and full-file compression method.
partcsv=ROOT/'exact-q6/gpu-partition-comparison.csv'
with partcsv.open(newline='') as f: q6rows=list(csv.DictReader(f))
refitcsv=ROOT/'endpoint-refit/comparison.csv'
with refitcsv.open(newline='') as f: erows=list(csv.DictReader(f))
fig,axs=plt.subplots(1,2,figsize=(9.5,4.4),layout='constrained')
for ax,scene in zip(axs,('dark','forest')):
 candidates=[]
 for r in q6rows:
  if r['source']==scene and r['variant'] in ('legacy-one-partition','projected-line'):
   candidates.append((r['variant'],int(r['full_astc_zstd3_bytes']),float(r['psnr_rgb_db'])))
 for r in erows:
  if r['source']==scene and r['variant']=='endpoint-refit':
   candidates.append((r['variant'],int(r['full_astc_zstd3_bytes']),float(r['rgb_psnr_db'])))
 for name,x,y in candidates:
  color,marker={'legacy-one-partition':('#555555','o'),'projected-line':('#1769aa','s'),'endpoint-refit':('#d1495b','D')}[name]
  ax.scatter(x/1000,y,color=color,marker=marker,s=72,label=name)
  dx=-8 if x>max(v[1] for v in candidates)-5 else 5
  ax.annotate(name.replace('-',' '),(x/1000,y),xytext=(dx,5),ha='right' if dx<0 else 'left',textcoords='offset points',fontsize=8)
 ax.set_title(f'{scene.title()} · exact 1920×1080 · q6')
 ax.set_xlabel('Full ASTC file Zstd3 size (kB, includes 16-byte header)');ax.set_ylabel('Decoded RGB PSNR (dB)');ax.grid(alpha=.22)
axs[0].legend(fontsize=8)
fig.suptitle('Endpoint refit: small quality changes at higher GPU cost',fontsize=12)
fig.savefig(ROOT/'figures/endpoint-refit-quality-vs-zstd3.png',dpi=180);plt.close(fig)
with (ROOT/'figures/endpoint-refit-quality-vs-zstd3.csv').open('w',newline='') as f:
 w=csv.writer(f);w.writerow(['source','variant','quality','full_astc_zstd3_bytes_including_16_byte_header','psnr_rgb_db'])
 for r in q6rows:
  if r['variant'] in ('legacy-one-partition','projected-line'):
   w.writerow([r['source'],r['variant'],r['q'],r['full_astc_zstd3_bytes'],r['psnr_rgb_db']])
 for r in erows:
  if r['variant']=='endpoint-refit':w.writerow([r['source'],r['variant'],r['quality'],r['full_astc_zstd3_bytes'],r['rgb_psnr_db']])
