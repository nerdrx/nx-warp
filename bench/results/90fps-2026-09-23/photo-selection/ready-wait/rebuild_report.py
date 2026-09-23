#!/usr/bin/env python3
import json,math,statistics
from pathlib import Path
import matplotlib.pyplot as plt
p=Path(__file__).parent;raw=json.loads((p/'windows.json').read_text())
def st(a):
 s=sorted(a);return {'count':len(s),'mean':statistics.mean(s),'median':statistics.median(s),'p95':s[min(len(s)-1,max(0,math.ceil(.95*len(s))-1))],'p95_definition':'nearest-rank ceil(0.95*n)-1'}
summary={'cases':{},'_method':{'p95':'nearest-rank ceil(0.95*n)-1','status':'exploratory single trial; not promoted'}}
for k,r in raw.items():
 sw=r['server_windows'];cw=r['client_windows'];
 pred=[sum(w['telemetry'][n] for n in ('wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms')) for w in cw if all(n in w.get('telemetry',{}) for n in ('wire_ms','queue_ms','decode_ms','decode_to_selection_ms','selection_to_predicted_ms'))]
 summary['cases'][k]={'encoder_fps':st([w['frames']/w['seconds'] for w in sw]),'new_source_fps':st([w['new_source']/w['seconds'] for w in cw]),'app_renderloop_fps':st([w['renderloop_fps'] for w in cw]),'wire_ms':st([w['telemetry']['wire_ms'] for w in cw]),'decode_to_selection_ms':st([w['telemetry']['decode_to_selection_ms'] for w in cw]),'own_gpu_ms':st([w['own_gpu_ms'] for w in cw if w.get('own_gpu_ms') is not None]),'predicted_runtime_first_arrival_to_prediction_ms':st(pred),'network_holes_total':sum(w['holes'] for w in r['network_windows'])}
(p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
ks=list(summary['cases']);labs=['0 ms','1 ms','2.5 ms'];x=range(3);fig,ax=plt.subplots(1,2,figsize=(9,4.8),dpi=170)
for i,(m,t,y) in enumerate([('new_source_fps','Fresh source rate','FPS'),('predicted_runtime_first_arrival_to_prediction_ms','Predicted-runtime estimate','ms')]):
 v=[summary['cases'][k][m]['mean'] for k in ks];ax[i].bar(x,v,color=['#777','#aa66cc','#7700ff']);ax[i].set_xticks(list(x),labs);ax[i].set_ylabel(y);ax[i].set_title(t);ax[i].grid(axis='y',alpha=.25)
 for z,a in zip(x,v):ax[i].text(z,a+.4,f'{a:.2f}',ha='center')
fig.suptitle('Ready-wait exploratory single trial — no promotion claim');fig.tight_layout(rect=(0,0,1,.93));fig.savefig(p/'comparison.png')
