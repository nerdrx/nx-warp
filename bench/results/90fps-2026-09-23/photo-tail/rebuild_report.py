#!/usr/bin/env python3
"""Rebuild the photo-tail summary and graph from sanitized windows.json."""
import json, math, statistics
from pathlib import Path
import matplotlib.pyplot as plt
p=Path(__file__).parent; raw=json.loads((p/'windows.json').read_text())
labels={'crowd-tail0':'crowd / tail0','crowd-tail64':'crowd / tail64','dark-tail0':'dark / tail0','dark-tail64':'dark / tail64','forest-tail0':'forest / tail0','forest-tail64':'forest / tail64'}
def stats(a):
 if not a: return {'count':0,'p95_definition':'nearest-rank ceil(0.95*n)-1'}
 a=sorted(a); return {'count':len(a),'mean':statistics.mean(a),'median':statistics.median(a),'p95':a[min(len(a)-1,max(0,math.ceil(.95*len(a))-1))],'p95_definition':'nearest-rank ceil(0.95*n)-1'}
summary={'cases':{},'_method':{'workload':'same static photo and same +/-8 px shifted paired view; tail64 adds 64 discarded 16-byte packets after each main frame','windowing':'sanitized retained complete 2-second windows; server first five complete windows already removed; only final <1.9s window removed; client begins upload marker +10s','payload_formula':'frames * bytes_per_frame * 8 / (seconds * 1e6)','predicted_runtime_formula':'wire + queue + decode + decode_to_selection + selection_to_predicted'}}
for k in labels:
 r=raw[k]; sw=r['server_windows']; cw=r['client_windows']; nw=r['network_windows']
 ef=[w['frames']/w['seconds'] for w in sw]; enc=[w['encode_ms'] for w in sw]; pay=[w['payload_mbps'] for w in sw]
 def tv(name): return [w['telemetry'][name] for w in cw if name in w.get('telemetry',{})]
 pred=[sum(w['telemetry'][n] for n in ('wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms')) for w in cw if all(n in w.get('telemetry',{}) for n in ('wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms'))]
 summary['cases'][k]={'label':labels[k],'encoder_fps':stats(ef),'encode_ms':stats(enc),'payload_mbps':stats(pay),'new_source_fps':stats([w['new_source']/w['seconds'] for w in cw]),'app_renderloop_fps':stats([w['renderloop_fps'] for w in cw]),'wire_ms':stats(tv('wire_ms')),'queue_ms':stats(tv('queue_ms')),'decode_ms':stats(tv('decode_ms')),'decode_to_selection_ms':stats(tv('decode_to_selection_ms')),'selection_to_predicted_ms':stats(tv('selection_to_predicted_ms')),'own_gpu_ms':stats([w['own_gpu_ms'] for w in cw if w.get('own_gpu_ms') is not None]),'predicted_runtime_first_arrival_to_prediction_ms':stats(pred),'network_holes_total':sum(w['holes'] for w in nw),'server_windows':len(sw),'client_windows':len(cw)}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
d=summary['cases']; keys=list(labels); x=range(6); labs=['crowd\n0','crowd\n64','dark\n0','dark\n64','forest\n0','forest\n64']
def mean(k,m): return d[k][m]['mean']
fig,ax=plt.subplots(1,3,figsize=(13,5.4),dpi=170)
for i,(metric,title,ylabel) in enumerate([('new_source_fps','Client new-source FPS','FPS'),('wire_ms','Wire / pipeline stages','ms'),('predicted_runtime_first_arrival_to_prediction_ms','Predicted-runtime estimate','ms')]):
 if metric=='wire_ms':
  names=['wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms']; vals=[[mean(k,n) for n in names] for k in keys]; bottom=[0]*6
  for n in range(5):
   v=[row[n] for row in vals]; ax[i].bar(x,v,bottom=bottom,label=names[n].replace('_ms','')); bottom=[a+b for a,b in zip(bottom,v)]
  ax[i].legend(fontsize=7)
 else:
  v=[mean(k,metric) for k in keys]; ax[i].bar(x,v,color=['#777','#7700ff']*3)
 ax[i].set_ylabel(ylabel); ax[i].set_title(title); ax[i].set_xticks(list(x),labs); ax[i].grid(axis='y',alpha=.25)
fig.suptitle('Photo-tail paired workload — window means; no photon claim'); fig.tight_layout(rect=(0,0,1,.94)); fig.savefig(p/'comparison.png')
