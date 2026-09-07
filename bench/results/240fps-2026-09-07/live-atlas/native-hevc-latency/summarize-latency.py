import csv,json,sys,numpy as np
from collections import defaultdict
for path in sys.argv[1:]:
 frames=defaultdict(dict)
 for row in csv.reader(open(path)):
  if len(row)<4 or row[3]!='0' or row[0] not in ['receive_begin','receive_end','decode_begin','decode_end','blit']: continue
  f=frames[int(row[1])]; t=int(row[2]); f[row[0]]=min(f.get(row[0],t),t)
 start=min(f['receive_begin'] for f in frames.values() if 'receive_begin' in f)
 warm=[f for f in frames.values() if f.get('receive_begin',0)>=start+10_000_000_000]
 result={'file':path,'stream':0,'warmup_s':10,'received_frames':len(warm),'stages':{}}
 for name,a,b in [('arrival_to_ready','receive_begin','decode_end'),('decoder_handoff_to_ready','decode_begin','decode_end'),('ready_to_render_selection','decode_end','blit'),('arrival_to_render_selection','receive_begin','blit')]:
  values=[(f[b]-f[a])/1e6 for f in warm if a in f and b in f]
  result['stages'][name]={'count':len(values),'negative':sum(v<0 for v in values),'p50_p95_p99_ms':np.percentile(values,[50,95,99]).round(3).tolist() if values else []}
 print(json.dumps(result))
