#!/usr/bin/env python3
"""Rebuild aggregate summary and controller-quality graph from sanitized windows.json."""
import json, math, statistics
from pathlib import Path
import matplotlib.pyplot as plt
p=Path(__file__).parent; raw=json.loads((p/'windows.json').read_text())
def stats(a):
 if not a:return {'count':0,'p95_definition':'nearest-rank ceil(0.95*n)-1'}
 s=sorted(a);return {'count':len(s),'mean':statistics.mean(s),'median':statistics.median(s),'p95':s[min(len(s)-1,max(0,math.ceil(.95*len(s))-1))],'p95_definition':'nearest-rank ceil(0.95*n)-1'}
summary={'cases':{},'_method':{'workload':'same crowd-auto500 photo paired workload; tail64 adds 64 discarded packets after each main frame','quality_budget':'direct target values are encoder control values, not link-capacity measurements','controller_order':'server logs have no timestamps; event order follows log order and is not a time axis','p95':'nearest-rank ceil(0.95*n)-1'}}
for k,r in raw.items():
 sw=r['server_windows'];cw=r['client_windows'];
 def tv(n):return [w['telemetry'][n] for w in cw if n in w.get('telemetry',{})]
 pred=[sum(w['telemetry'][n] for n in ('wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms')) for w in cw if all(n in w.get('telemetry',{}) for n in ('wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms'))]
 summary['cases'][k]={'encoder_fps':stats([w['frames']/w['seconds'] for w in sw]),'encode_ms':stats([w['encode_ms'] for w in sw]),'payload_mbps':stats([w['payload_mbps'] for w in sw]),'new_source_fps':stats([w['new_source']/w['seconds'] for w in cw]),'app_renderloop_fps':stats([w['renderloop_fps'] for w in cw]),'wire_ms':stats(tv('wire_ms')),'own_gpu_ms':stats([w['own_gpu_ms'] for w in cw if w.get('own_gpu_ms') is not None]),'predicted_runtime_first_arrival_to_prediction_ms':stats(pred),'network_holes_total':sum(w['holes'] for w in r['network_windows']),'server_windows':len(sw),'client_windows':len(cw)}
summary['controller_events']={k:r['controller_events'] for k,r in raw.items()}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
fig,ax=plt.subplots(1,2,figsize=(12,5.2),dpi=170)
for k,color,label in [('crowd-tail0','#777','tail0'),('crowd-tail64','#7700ff','tail64')]:
 ev=raw[k]['controller_events']; ax[0].plot([e['order'] for e in ev],[e['to_target_mbps'] for e in ev],marker='o',ms=3,color=color,label=label)
for k,color,label in [('crowd-tail0','#777','tail0'),('crowd-tail64','#7700ff','tail64')]:
 c=summary['cases'][k]; ax[1].bar(('tail0','tail64').index(label),c['new_source_fps']['mean'],color=color,label=label)
 ax[1].text(('tail0','tail64').index(label),c['new_source_fps']['mean']+.8,f"{c['new_source_fps']['mean']:.1f}",ha='center')
ax[0].set_title('Encoder quality-budget target trajectory');ax[0].set_xlabel('controller event order (not seconds)');ax[0].set_ylabel('target control value (Mbit/s)');ax[0].legend();ax[0].grid(alpha=.25)
ax[1].set_title('Client new-source rate');ax[1].set_ylabel('FPS');ax[1].set_xticks([0,1],['tail0','tail64']);ax[1].set_ylim(0,90);ax[1].grid(axis='y',alpha=.25)
fig.suptitle('Auto bitrate paired photo workload — no link-capacity claim');fig.tight_layout(rect=(0,0,1,.94));fig.savefig(p/'comparison.png')
