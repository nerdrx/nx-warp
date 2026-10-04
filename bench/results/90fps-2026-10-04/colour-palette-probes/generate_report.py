#!/usr/bin/env python3
"""Build compact, source-linked ASTC probe report from existing offline outputs."""
from collections import Counter
from pathlib import Path
import csv, hashlib, json, subprocess
from PIL import Image, ImageDraw, ImageFont
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SCRATCH = Path('/run/media/nerdrx/Lex/claude/nx-scratch')
OUT = Path(__file__).resolve().parent
SOURCE_MANIFEST = SCRATCH / 'astc-native-colour-20261004/manifest.json'
PROJECTED = SCRATCH / 'astc-projected-line-20261004/results'
QUEUE = SCRATCH / 'astc-partition-queue-20261004'
COARSE = SCRATCH / 'astc-coarse-grid-20261004'
DUAL = SCRATCH / 'astc-dualplane-20261004'
ZSTD_VERSION = subprocess.run(['zstd', '--version'], text=True, capture_output=True, check=True).stdout.strip()

photo_manifest = json.loads(SOURCE_MANIFEST.read_text())
fixture = {x['name']: x for x in photo_manifest['fixtures']}
queue_rows = {r['scene']: r for r in csv.DictReader((QUEUE/'results/exact-user-final/comparison.csv').open())}
coarse_rows = {r['scene']: r for r in csv.DictReader((COARSE/'comparison.csv').open())}
dp_q6 = {r['scene']: r for r in json.loads((DUAL/'results/threshold-sweep.json').read_text())['records'] if r['threshold_pct'] == 20.0}
dp_new = json.loads((DUAL/'results/chroma-q2-q4.json').read_text())['records']
chroma_top5 = {(r['scene'], r['baseline_q']): r for r in dp_new if r['grid']=='4x4' and r['chroma_top_pct']==5}
dp_full = {r['scene']: r for r in json.loads((DUAL/'results/quality.json').read_text())['records']}

# Check source hashes against the user-photo provenance manifest.
def sha_file(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
for scene in ('dark', 'forest'):
    raw = Path(fixture[scene]['rgba_raw'])
    assert sha_file(raw) == fixture[scene]['rgba_sha256']

# Normalize compression comparisons: Zstd 1.5.7 -3 over each complete ASTC file, header included.
def zstd_size(path):
    return len(subprocess.run(['zstd', '-3', '-q', '-c', str(path)], stdout=subprocess.PIPE, check=True).stdout)
def block_modes(path):
    data=Path(path).read_bytes()
    assert data[:4] == bytes((0x13,0xAB,0xA1,0x5C)) and (len(data)-16)%16 == 0
    return Counter(int.from_bytes(data[i:i+2], 'little') & 0x7ff for i in range(16,len(data),16))

def add_row(scene, q, variant, family, mode, astc, psnr, selected, total, detail, gpu_med='', gpu_p95='', source='exact-user-photo'):
    base_astc=PROJECTED/f'{scene}-projected-q{q}.astc'
    base_psnr=float(queue_rows[scene]['baseline_psnr_db']) if q==6 else float(chroma_top5[(scene,q)]['baseline_psnr_db'])
    bs=zstd_size(base_astc); cs=zstd_size(astc)
    rows.append({
      'scene':scene,'cohort':source,'q':q,'variant':variant,'engine_family':family,
      'legal_mode':mode,'baseline_mode':'0x0F3 (8x8, 5x5 weights, CEM8, 1 partition)',
      'baseline_psnr_db':f'{base_psnr:.6f}','candidate_psnr_db':f'{float(psnr):.6f}',
      'delta_psnr_db':f'{float(psnr)-base_psnr:.6f}','baseline_zstd3_full_astc_bytes':bs,
      'candidate_zstd3_full_astc_bytes':cs,'zstd_delta_bytes':cs-bs,
      'zstd_growth_pct':f'{100*(cs-bs)/bs:.6f}','selected_candidate_blocks':selected,
      'total_blocks':total,'selection_detail':detail,'gpu_median_ms':gpu_med,'gpu_p95_ms':gpu_p95,
      'cpu_wall_time_ms':'','astc_path':str(astc),'source_rgba_sha256':fixture[scene]['rgba_sha256'] if scene in fixture else ''})

rows=[]
for scene in ('dark','forest'):
    total=32400
    # Primary: new 6-bit endpoint, top-5% chroma-targeted 4x4 dual-plane branch.
    for q in (2,4,6):
        m=chroma_top5[(scene,q)]
        add_row(scene,q,f'Projected q{q} baseline','reference','0x0F3',PROJECTED/f'{scene}-projected-q{q}.astc',
                m['baseline_psnr_db'],0,total,'matched q-specific projected-PCA reference')
        astc=DUAL/f'results/{scene}-q{q}-4x4-top5.astc'
        add_row(scene,q,'dual-plane-4x4 top-5% chroma / 6-bit endpoints','CPU probe','0x442',astc,
                m['gated_psnr_db'],m['selected_blocks'],total,
                'top 5% var(R-G)+var(B-G); require decoded RGB SSE <=95% of q-specific projected baseline')
    if scene=='dark':
        qrow=queue_rows[scene]; astc=QUEUE/'results/exact-user-final/dark-queue.astc'
        add_row(scene,6,'single-seed queued CEM8 2-partition','GPU timestamp probe','0x053',astc,qrow['queue_psnr_db'],qrow['astc_mode_0x053_blocks'],total,
                'residual >=8192 and >=5% lower decoded MSE; one seed',qrow['queue_gpu_median_ms'],qrow['queue_gpu_p95_ms'])
    qrow=queue_rows[scene]
    if scene=='forest':
        astc=QUEUE/'results/exact-user-final/forest-queue.astc'
        add_row(scene,6,'single-seed queued CEM8 2-partition','GPU timestamp probe','0x053',astc,qrow['queue_psnr_db'],qrow['astc_mode_0x053_blocks'],total,
                'residual >=8192 and >=5% lower decoded MSE; one seed',qrow['queue_gpu_median_ms'],qrow['queue_gpu_p95_ms'])
    c=coarse_rows[scene]; astc=COARSE/f'results/{scene}-coarse-grid-fallback.astc'
    add_row(scene,6,'3x3-grid two-partition CEM8 fallback','CPU probe','0x1BF',astc,c['candidate_full_psnr_db'],c['candidate_winner_tiles'],total,
            f"orthogonal residual >=8192; {c['dense_tiles_gate']} tiles gated, top two of 1024 seeds, 24 ALS steps")
    # Earlier broad 20% decoded-SSE gate; distinguish from primary top-5 chroma follow-up.
    s=dp_q6[scene]; astc=DUAL/f"results/{scene}-threshold-20.astc"
    add_row(scene,6,'dual-plane 4x4 broad 20% SSE gate / 8-bit endpoints','CPU probe','0x442',astc,s['psnr_db'],s['selected_blocks'],total,
            'candidate block SSE <=80% of projected baseline')
    full=dp_full[scene]; astc=DUAL/f'results/{scene}-dualplane.astc'
    add_row(scene,6,'dual-plane 4x4 ungated / 8-bit endpoints','CPU probe','0x442',astc,full['dualplane']['psnr_db'],total,total,
            'all 32,400 blocks encoded; no per-block fallback')

# Separate legacy crowd smoke row. It never enters matched-photo charts.
crowd=queue_rows['crowd-old-512']; crowd_base=QUEUE/'results/crowd-old-baseline.astc'; crowd_cand=QUEUE/'results/exact-user-final/crowd-old-512-queue.astc'
rows.append({'scene':'crowd-old-512','cohort':'legacy crop, provenance/foveation unresolved','q':6,
 'variant':'single-seed queued CEM8 2-partition','engine_family':'GPU timestamp probe','legal_mode':'0x053',
 'baseline_mode':'0x0F3 (legacy crop run)','baseline_psnr_db':f"{float(crowd['baseline_psnr_db']):.6f}",'candidate_psnr_db':f"{float(crowd['queue_psnr_db']):.6f}",
 'delta_psnr_db':f"{float(crowd['psnr_gain_db']):.6f}",'baseline_zstd3_full_astc_bytes':zstd_size(crowd_base),'candidate_zstd3_full_astc_bytes':zstd_size(crowd_cand),
 'zstd_delta_bytes':zstd_size(crowd_cand)-zstd_size(crowd_base),'zstd_growth_pct':f"{100*(zstd_size(crowd_cand)-zstd_size(crowd_base))/zstd_size(crowd_base):.6f}",
 'selected_candidate_blocks':crowd['astc_mode_0x053_blocks'],'total_blocks':4096,'selection_detail':'smoke only; old crop provenance and foveation status unknown',
 'gpu_median_ms':crowd['queue_gpu_median_ms'],'gpu_p95_ms':crowd['queue_gpu_p95_ms'],'cpu_wall_time_ms':'','astc_path':str(crowd_cand),'source_rgba_sha256':crowd['source_rgba_sha256']})

# Validate actual legal block-mode fields and selection counts.
checks=[]
def check_modes(path, expected, expected_count=None):
    c=block_modes(path)
    checks.append({'path':str(path),'mode_counts':{f'0x{k:03X}':v for k,v in sorted(c.items())}})
    if expected_count is not None: assert c.get(expected,0)==expected_count, (path,c,expected,expected_count)
    assert expected in c, (path,c,expected)
    return c
for scene in ('dark','forest'):
    base=PROJECTED/f'{scene}-projected-q6.astc'; check_modes(base,0x0f3,32400)
    check_modes(QUEUE/f'results/exact-user-final/{scene}-queue.astc',0x053,int(queue_rows[scene]['astc_mode_0x053_blocks']))
    check_modes(COARSE/f'results/{scene}-coarse-grid-fallback.astc',0x1bf,int(coarse_rows[scene]['candidate_winner_tiles']))
    check_modes(DUAL/f'results/{scene}-threshold-20.astc',0x442,int(dp_q6[scene]['selected_blocks']))
    check_modes(DUAL/f'results/{scene}-dualplane.astc',0x442,32400)
    for q in (2,4,6): check_modes(DUAL/f'results/{scene}-q{q}-4x4-top5.astc',0x442,int(chroma_top5[(scene,q)]['selected_blocks']))

cols=['scene','cohort','q','variant','engine_family','legal_mode','baseline_mode','baseline_psnr_db','candidate_psnr_db','delta_psnr_db','baseline_zstd3_full_astc_bytes','candidate_zstd3_full_astc_bytes','zstd_delta_bytes','zstd_growth_pct','selected_candidate_blocks','total_blocks','selection_detail','gpu_median_ms','gpu_p95_ms','cpu_wall_time_ms','astc_path','source_rgba_sha256']
with (OUT/'metrics.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=cols,extrasaction='ignore',lineterminator='\n'); w.writeheader(); w.writerows(rows)

# Crop-only photo panels: exact source, baseline, and three candidate families; never save full photos.
source_paths={'dark':Path(fixture['dark']['png']),'forest':Path(fixture['forest']['png'])}
rgba_paths={
 'dark':[
  PROJECTED/'dark-projected-q6.rgba',
  QUEUE/'results/exact-user-final/dark-queue-decoded.rgba',
  COARSE/'results/dark-coarse-grid-fallback.rgba',
  DUAL/'results/dark-q6-4x4-top5.rgba'],
 'forest':[
  PROJECTED/'forest-projected-q6.rgba',
  QUEUE/'results/exact-user-final/forest-queue-decoded.rgba',
  COARSE/'results/forest-coarse-grid-fallback.rgba',
  DUAL/'results/forest-q6-4x4-top5.rgba']}
labels=['Provided-photo source','Projected q6 baseline','Historical GPU queue (UB)','CPU 3x3 CEM8 0x1BF','CPU chroma top-5% 0x442']
rects={'dark':(320,240,512,512),'forest':(704,128,512,512)}
font_path=Path('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf')
font=ImageFont.truetype(str(font_path),18) if font_path.exists() else ImageFont.load_default()
for scene in ('dark','forest'):
    x,y,w,h=rects[scene]
    base=Image.open(source_paths[scene]).convert('RGB').crop((x,y,x+w,y+h))
    panels=[base]
    for raw in rgba_paths[scene]:
        im=Image.frombytes('RGBA',(1920,1080),raw.read_bytes()).convert('RGB').crop((x,y,x+w,y+h))
        panels.append(im)
    canvas=Image.new('RGB',(w*len(panels),h+42),'white'); d=ImageDraw.Draw(canvas)
    for i,(im,label) in enumerate(zip(panels,labels)):
        x0=i*w; d.text((x0+8,8),label,font=font,fill='#18233a'); canvas.paste(im,(x0,42))
    canvas.save(OUT/f'{scene}-crop-panel.png',optimize=True)

# Plot q6 exact-photo quality vs one normalized compressed-size metric; time is a separate panel.
variants=[
 ('Projected q6 baseline','Projected q6',0x0f3,'#53657d','o'),
 ('single-seed queued CEM8 2-partition','Queue 0x053',0x053,'#d07a37','o'),
 ('3x3-grid two-partition CEM8 fallback','3x3 CEM8',0x1bf,'#41956b','^'),
 ('dual-plane 4x4 ungated / 8-bit endpoints','DP ungated',0x442,'#8665b0','^'),
 ('dual-plane 4x4 broad 20% SSE gate / 8-bit endpoints','DP 20% gate',0x442,'#d05763','^'),
 ('dual-plane-4x4 top-5% chroma / 6-bit endpoints','DP top5 chroma',0x442,'#118b90','^'),
]
fig=plt.figure(figsize=(13,8.3),layout='constrained')
gs=fig.add_gridspec(2,2,height_ratios=[1.15,0.85])
for j,scene in enumerate(('dark','forest')):
    ax=fig.add_subplot(gs[0,j]); ax.set_title(scene.capitalize()+' photo · q6',loc='left',fontweight='bold')
    for variant,label,mode,color,marker in variants:
        r=next(x for x in rows if x['scene']==scene and x['q']==6 and x['variant']==variant)
        x=int(r['candidate_zstd3_full_astc_bytes'])/1000; y=float(r['candidate_psnr_db'])
        ax.scatter(x,y,s=72,c=color,marker=marker,edgecolor='white',linewidth=.7,zorder=3,label=label if j==0 else None)
        short={'Projected q6 baseline':'base','single-seed queued CEM8 2-partition':'queue','3x3-grid two-partition CEM8 fallback':'3x3','dual-plane 4x4 ungated / 8-bit endpoints':'all-DP','dual-plane 4x4 broad 20% SSE gate / 8-bit endpoints':'DP20','dual-plane-4x4 top-5% chroma / 6-bit endpoints':'chroma5'}[variant]
        dy={'base':.025,'queue':.06,'3x3':-.06,'all-DP':.08,'DP20':.025,'chroma5':-.08}[short]
        ax.annotate(short,(x,y),xytext=(4,6 if dy>0 else -10),textcoords='offset points',fontsize=8,color=color)
    b=next(x for x in rows if x['scene']==scene and x['q']==6 and x['variant']=='Projected q6 baseline')
    ax.axhline(float(b['candidate_psnr_db']),color='#8b97a8',linestyle=':',linewidth=1)
    ax.set_xlabel('Zstd -3 bytes, complete ASTC file (kB)'); ax.grid(True,alpha=.25)
    if j==0: ax.set_ylabel('External-decoded full-photo PSNR (dB)')
    if j==0: ax.legend(loc='best',fontsize=8,frameon=True)

ax=fig.add_subplot(gs[1,0]); ax.set_title('Measured GPU encode time only · RX 7900 XTX',loc='left',fontweight='bold')
labels=[]; med=[]; p95=[]; colors=[]
for scene in ('dark','forest'):
    r=queue_rows[scene]
    for name,field,color in [('baseline','baseline','#738197'),('queue','queue','#d07a37')]:
        labels.append(f'{scene} {name}')
        med.append(float(r[f'{field}_gpu_median_ms']))
        p95.append(float(r[f'{field}_gpu_p95_ms']))
        colors.append(color)
xx=list(range(len(labels))); err=[max(0,p-m) for p,m in zip(p95,med)]
ax.bar(xx,med,yerr=err,capsize=3,color=colors,width=.62)
for i,(m,p) in enumerate(zip(med,p95)): ax.text(i,m+.008,f'{m:.3f}/{p:.3f} ms',ha='center',va='bottom',fontsize=8)
ax.set_xticks(xx,labels,rotation=15,ha='right'); ax.set_ylabel('GPU timestamp (ms; median, p95)'); ax.grid(axis='y',alpha=.25)
ax=fig.add_subplot(gs[1,1]); ax.axis('off'); ax.set_title('CPU probes · timing not recorded',loc='left',fontweight='bold')
ax.text(0.02,.84,'3×3 CEM8: 1,510 / 246 tiles gated; 312 / 58 replacements.\nTop 2 of 1,024 seeds; 24 alternating fit steps.',fontsize=10,va='top')
ax.text(0.02,.57,'Dual-plane 20% SSE gate: 19,557 / 22,415 selected blocks;\nlarge Zstd growth. New chroma top-5% mode: 757 / 107 q6 blocks.',fontsize=10,va='top')
ax.text(0.02,.28,'CPU wall time and GPU-port cost are unavailable. Do not infer them\nfrom selection counts or fitting steps. Queue timings are PC shader-only.',fontsize=10,va='top',color='#9a3d34')
fig.suptitle('ASTC q6 trade-offs on matched exact photos',fontsize=16,fontweight='bold')
fig.savefig(OUT/'tradeoffs.svg',format='svg')
plt.close(fig)
svg=OUT/'tradeoffs.svg'
svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines())+'\n')

# Keep a compact crowd smoke row separate from exact-photo quality charts.
# Report source and result checksums, all LF line endings.
source_paths_to_hash={
 'source_manifest':SOURCE_MANIFEST,
 'queue_comparison':QUEUE/'results/exact-user-final/comparison.csv',
 'queue_manifest':QUEUE/'results/exact-user-final/manifest.json',
 'queue_readme':QUEUE/'README.md',
 'coarse_comparison':COARSE/'comparison.csv',
 'coarse_manifest':COARSE/'manifest.json',
 'coarse_readme':COARSE/'README.md',
 'dual_quality':DUAL/'results/quality.json',
 'dual_gate20_sweep':DUAL/'results/threshold-sweep.json',
 'dual_chroma_sweep':DUAL/'results/chroma-q2-q4.json',
 'dual_chroma_crops':DUAL/'results/chroma-native-crops.json',
 'dual_readme':DUAL/'README.md',
 'zstd_executable':Path(subprocess.run(['which','zstd'],text=True,capture_output=True,check=True).stdout.strip()),
}
with (OUT/'checksums.csv').open('w',newline='') as f:
    w=csv.writer(f,lineterminator='\n'); w.writerow(['role','path','sha256','size_bytes'])
    for k,p in source_paths_to_hash.items(): w.writerow([k,str(p),sha_file(p),p.stat().st_size])
    for p in [*source_paths.values(),*rgba_paths['dark'],*rgba_paths['forest'],
              PROJECTED/'dark-projected-q6.astc',PROJECTED/'forest-projected-q6.astc',
              QUEUE/'results/exact-user-final/dark-queue.astc',QUEUE/'results/exact-user-final/forest-queue.astc',
              COARSE/'results/dark-coarse-grid-fallback.astc',COARSE/'results/forest-coarse-grid-fallback.astc',
              DUAL/'results/dark-q6-4x4-top5.astc',DUAL/'results/forest-q6-4x4-top5.astc']:
        w.writerow(['source_artifact',str(p),sha_file(p),p.stat().st_size])
    for p in sorted(OUT.iterdir()):
        if p.name in ('checksums.csv','manifest.json') or not p.is_file(): continue
        w.writerow(['report_output',str(p),sha_file(p),p.stat().st_size])

manifest={
 'created_date':'2026-10-04','purpose':'Offline matched-source comparison of ASTC palette/partition probes; no production/runtime claims.',
 'source_identity':{s:{'path':fixture[s]['png'],'png_sha256':fixture[s]['png_sha256'],'rgba_sha256':fixture[s]['rgba_sha256'],'dimensions':'1920x1080','origin_manifest':str(SOURCE_MANIFEST)} for s in ('dark','forest')},
 'compression_normalization':{'tool':ZSTD_VERSION,'level':3,'input':'complete .astc file including 16-byte header','used_for_cross-probe_metrics':True},
 'mode_verification':checks,
 'crowd_limit':'Queue crowd-old-512 row is smoke-only. Source/origin and foveation status are unresolved; excluded from matched-photo chart.',
 'timing_limits':{'gpu':'Historical queue probe only: RX 7900 XTX, 12 warmups, 30 samples. Later review found undefined padding-grid accesses; measurements do not validate a corrected shader.','cpu':'Coarse-grid and dual-plane probes have no measured CPU wall time or GPU-port cost.'},
 'limits':['q6 cross-probe PSNR scores are from external Basis ASTC LDR decoding against the exact full-resolution source photo.',
           'Chroma top-5% q2/q4/q6 points use the q-specific projected baseline and external decode.',
           'CPU prototype quality and counts do not establish GPU speed or live bitrate.',
           'No source photos, full decoded frames, ASTC fixtures, executables, builds, or runtime artifacts are included; only small photo crops and report outputs are saved.'],
 'inputs_checksums':'checksums.csv','metrics_csv':'metrics.csv','methods':'README.md'}
(OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

# Source checksums include only report generator and text/visual outputs; manifests are separate to avoid self-hashing.
print(json.dumps({'out':str(OUT),'rows':len(rows),'modes_checked':len(checks),'zstd':ZSTD_VERSION,'crops':['dark-crop-panel.png','forest-crop-panel.png'],'figure':'tradeoffs.svg','manifest':'manifest.json'},indent=2))
