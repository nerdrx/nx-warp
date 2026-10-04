from pathlib import Path
import re, csv, json, hashlib, shutil
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SCR=Path('/run/media/nerdrx/Lex/claude/nx-scratch/astc-newest-pair-20261004')
CAP=SCR/'zero-delay/live-check'
REPO=Path('/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe')
OUT=Path(__file__).resolve().parent
sets=[('newest-zero-a','newest-zero-a','newest'),('nearest-zero-b','nearest-zero-b','nearest'),('newest-zero-c','newest-zero-c','newest')]
rx={
 'iter':re.compile(r'(\d+) iterations in 2\.0 s \(([\d.]+)/s\).*?(\d+) new-source'),
 'older':re.compile(r'selected older than available (\d+)'),
 'gpu':re.compile(r"this app's own GPU pass ([\d.]+) ms per iteration"),
 'hold':re.compile(r'decode->selection ([\d.]+)'),
 'fps':re.compile(r'(\d+) iterations in 2\.0 s \(([\d.]+)/s\)'),
 'new':re.compile(r'(\d+) new-source'),
}
rows=[]; inputs=[]
for name,folder,mode in sets:
 d=CAP/folder
 meta=json.loads((d/'metadata.json').read_text())
 lines=(d/'client-extract.txt').read_text().splitlines()
 current=None
 def emit():
  if current and 'fps' in current: rows.append(current.copy())
 for line in lines:
  m=rx['iter'].search(line)
  if m:
   emit(); current={'capture':name,'selection':mode,'apk_sha256':meta['apk_sha256'],'start_utc':meta['start_utc'],'window_utc':line[ line.find('[2026-'):line.find('] [WiVRn]') ],'iterations':int(m.group(1)),'fps':float(m.group(2)),'new_source':int(m.group(3))}
   continue
  if current:
   for key in ('older','gpu','hold'):
    m=rx[key].search(line)
    if m: current[key]=int(m.group(1)) if key=='older' else float(m.group(1))
 emit()
 for key in ('metadata.json','client-extract.txt','server-extract.txt'):
  src=d/key; dst=OUT/f'{name}-{key}'
  shutil.copyfile(src,dst); inputs.append({'path':str(src),'sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'size_bytes':src.stat().st_size,'report_copy':dst.name})
assert len(rows)==22, len(rows)
with (OUT/'windows.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=['capture','selection','apk_sha256','start_utc','window_utc','iterations','fps','new_source','older','gpu','hold'],lineterminator='\n'); w.writeheader(); w.writerows(rows)

# Plot keeps capture windows discrete; no averaging of resume transitions.
fig,axs=plt.subplots(2,1,figsize=(10,6.3),sharex=True,layout='constrained')
colors={'newest-zero-a':'#25845c','nearest-zero-b':'#bb6e24','newest-zero-c':'#306ba2'}
for capture,_,_ in sets:
 rs=[r for r in rows if r['capture']==capture]
 x=list(range(1,len(rs)+1)); col=colors[capture]
 axs[0].plot(x,[r.get('older',0) for r in rs],marker='o',label=capture,color=col)
 axs[1].plot(x,[r.get('gpu',float('nan')) for r in rs],marker='o',label=capture+' · app GPU',color=col)
 axs[1].plot(x,[r.get('hold',float('nan')) for r in rs],marker='x',linestyle='--',alpha=.72,label=capture+' · decode→selection',color=col)
axs[0].set_ylabel('Selected older-than-available\n(count per 2 s window)')
axs[0].set_title('ASTC stereo-pair selection at measured zero de-jitter delay',loc='left',fontweight='bold')
axs[0].grid(alpha=.25); axs[0].legend(ncol=3,fontsize=8)
axs[1].set_ylabel('Milliseconds per window mean'); axs[1].set_xlabel('Window order within each capture (not aligned in time)')
axs[1].grid(alpha=.25); axs[1].legend(ncol=3,fontsize=8)
fig.savefig(OUT/'freshness-and-pipeline.png',dpi=150); plt.close(fig)

source_files={
 'selection_diff':REPO/'client/scenes/stream.cpp',
 'selection_documentation':REPO/'docs/ASTC_FRAME_SELECTION.md',
 'apk_manifest':SCR/'zero-delay/packaging/manifest.json',
 'capture_tool':SCR/'zero-delay/capture.py',
}
for p in source_files.values(): inputs.append({'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'size_bytes':p.stat().st_size})
for out in [OUT/'windows.csv',OUT/'freshness-and-pipeline.png',OUT/'selection-documentation.md',OUT/'selection-diff.patch',OUT/'README.md',OUT/'build_report.py']:
 inputs.append({'path':str(out),'sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'size_bytes':out.stat().st_size})
(OUT/'manifest.json').write_text(json.dumps({'title':'ASTC zero-delay frame freshness','created':'2026-10-04','apk_sha256':rows[0]['apk_sha256'],'capture_metadata':[json.loads((CAP/n/'metadata.json').read_text()) for n,_,_ in sets],'source_and_report_hashes':inputs,'window_rows':len(rows),'notes':['All three measured captures used the same installed APK hash.','newest-zero-c begins with one mixed resume window; retain its older selection count of 86 and decode-to-selection mean 11.4 ms. Six following windows report zero older selections.','nearest-zero-b often still has 7–8 ms decode-to-selection despite a small older-selection count; no fixed latency saving is supported.','short stationary off-head wake captures only; no motion, sustained-complex-scene, perceived-quality or photon-latency evidence.']},indent=2)+'\n')
print(json.dumps({'rows':len(rows),'groups':{n:sum(r['capture']==n for r in rows) for n,_,_ in sets},'apk':rows[0]['apk_sha256'],'out':str(OUT)},indent=2))
