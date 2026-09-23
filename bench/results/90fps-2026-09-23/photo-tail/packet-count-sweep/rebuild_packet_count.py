#!/usr/bin/env python3
"""Regenerate packet-count sweep summary and graph from sanitized windows.json."""
import json,math,statistics
from pathlib import Path
import matplotlib.pyplot as plt
p=Path(__file__).parent;raw=json.loads((p/'windows.json').read_text())
def st(a):
 s=sorted(a); return {'count':len(s),'mean':statistics.mean(s),'median':statistics.median(s),'p95':s[min(len(s)-1,max(0,math.ceil(.95*len(s))-1))],'p95_definition':'nearest-rank ceil(0.95*n)-1'}
summary={'cases':{},'_method':{'comparison':'separate packet-count sweep; not the original paired tail0/tail64 report','p95':'nearest-rank ceil(0.95*n)-1'}}
for k,r in raw.items():
 sw=r['server_windows'];cw=r['client_windows'];
 def tv(n):return [w['telemetry'][n] for w in cw if n in w.get('telemetry',{})]
 pred=[sum(w['telemetry'][n] for n in ('wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms')) for w in cw if all(n in w.get('telemetry',{}) for n in ('wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms'))]
 summary['cases'][k]={'encoder_fps':st([w['frames']/w['seconds'] for w in sw]),'encode_ms':st([w['encode_ms'] for w in sw]),'payload_mbps':st([w['payload_mbps'] for w in sw]),'new_source_fps':st([w['new_source']/w['seconds'] for w in cw]),'app_renderloop_fps':st([w['renderloop_fps'] for w in cw]),'wire_ms':st(tv('wire_ms')),'own_gpu_ms':st([w['own_gpu_ms'] for w in cw if w.get('own_gpu_ms') is not None]),'predicted_runtime_first_arrival_to_prediction_ms':st(pred),'network_holes_total':sum(w['holes'] for w in r['network_windows'])}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
ks=list(summary['cases']); labels=[k.rsplit('tail',1)[1] for k in ks]; x=range(len(ks)); fig,ax=plt.subplots(1,2,figsize=(10,5),dpi=170)
for i,(m,title) in enumerate([('new_source_fps','Client new-source FPS'),('predicted_runtime_first_arrival_to_prediction_ms','Predicted-runtime estimate')]):
 v=[summary['cases'][k][m]['mean'] for k in ks]; ax[i].bar(x,v,color=['#777','#888','#aa66cc','#7700ff']); ax[i].set_xticks(list(x),labels); ax[i].set_ylabel('FPS' if i==0 else 'ms'); ax[i].set_title(title); ax[i].grid(axis='y',alpha=.25)
 for z,y in zip(x,v):ax[i].text(z,y+.8,f'{y:.1f}',ha='center')
fig.suptitle('Separate crowd500 packet-count sweep — no unqualified win');fig.tight_layout(rect=(0,0,1,.94));fig.savefig(p/'comparison.png')
