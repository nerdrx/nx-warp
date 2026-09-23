#!/usr/bin/env python3
from pathlib import Path
import csv,json,statistics
import matplotlib.pyplot as plt
out=Path(__file__).resolve().parent
rows=list(csv.DictReader((out/'metrics.csv').open()))
for r in rows:
 for k in ('run','n_windows'): r[k]=int(r[k])
 for k in ('mean','min_window','median_window','p95_window','max_window'): r[k]=float(r[k])
windows=list(csv.DictReader((out/'windows.csv').open()))
for r in windows:
 r['run']=int(r['run']); r['holes']=int(r['holes'])
 for k in ('payload_mbps','fresh_fps','decode_ms','derived_software_delay_ms','wire_ms_min'): r[k]=float(r[k])
ab=[]
for case in ('A','B'):
 for metric in ('codec_wire_mbps','server_encode_ms','decode_ms','fresh_fps','derived_software_delay_ms','wire_ms'):
  v=[r['mean'] for r in rows if r['case']==case and r['metric']==metric]
  ab.append({'case':case,'metric':metric,'runs':len(v),'mean_of_run_means':statistics.mean(v),'min_run_mean':min(v),'max_run_mean':max(v)})
A=next(x['mean_of_run_means'] for x in ab if x['case']=='A' and x['metric']=='codec_wire_mbps'); B=next(x['mean_of_run_means'] for x in ab if x['case']=='B' and x['metric']=='codec_wire_mbps')
summary={'scope':'sanitized current pointer live ABBA','implementation_label':'A=cache0 + predictor0; B=cache1 + predictor1; scalar decode','experiment':'fixed 500 Mbps requested (433.604 codec budget), 90 Hz, JIT 45000 us, window 0, tail 64, 120 s each','fixture':'dark duplicated photo with shifted8 every 4 frames','cutoff':'source+10 s','case_definition':'A=baseline (cache0,predictor0), repeats 1 and 4; B=cache+predictor (cache1,predictor1), repeats 2 and 3','payload_definition':'full-frame codec output including detail and safety; excludes transport/FEC/padding','byte_win_pct':100*(A-B)/A,'abba_means':ab,'windows':windows,'baseline_comparison':'Old scalar results are a distinct earlier experiment and are not a direct paired baseline here.','quality_note':'No quality change is inferred; byte exactness is covered by separate GPU fixture evidence. See ../compression-report/README.md.','promotion_note':'Predictor remains opt-in; this report does not promote a default.','network_note':'Both A and B contain network dips; inspect minimum-window wire/fresh values, not means alone.'}
(out/'abba_means.csv').write_text('case,metric,runs,mean_of_run_means,min_run_mean,max_run_mean\n'+'\n'.join(','.join(str(x[k]) for k in ('case','metric','runs','mean_of_run_means','min_run_mean','max_run_mean')) for x in ab)+'\n')
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
fig,axs=plt.subplots(2,2,figsize=(10,7),constrained_layout=True)
for ax,metric,title,ylabel in zip(axs.flat,('codec_wire_mbps','server_encode_ms','decode_ms','fresh_fps'),('Codec payload','Server encode','Client decode','Fresh frames'),('Mbps (detail+safety payload)','ms','ms','FPS')):
 for case,color,label in (('A','#377eb8','A baseline: cache0 + predictor0'),('B','#e41a1c','B cache+predictor: cache1 + predictor1')):
  rr=sorted((x for x in rows if x['case']==case and x['metric']==metric),key=lambda x:x['run'])
  ax.plot([x['run'] for x in rr],[x['mean'] for x in rr],'o-',color=color,label=label)
 ax.set_title(title); ax.set_xlabel('repeat'); ax.set_ylabel(ylabel); ax.grid(alpha=.25)
axs[0,0].legend(fontsize=7)
fig.suptitle('Pointer live ABBA: baseline vs cache+predictor (2 s windows)')
fig.savefig(out/'comparison.png',dpi=160);fig.savefig(out/'comparison.svg');plt.close(fig)
