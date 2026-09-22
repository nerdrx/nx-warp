import json,re,statistics
from pathlib import Path
base=Path('/run/media/nerdrx/Lex/claude/nx-scratch/live/direct-20260922'); out=Path('/run/media/nerdrx/Lex/claude/nx-warp/bench/results/90fps-2026-09-23/wire-admission')
def mid(a): return a[5:-1] if len(a)>6 else []
def parse(tag):
 s=(base/f'server-fixed25-{tag}-admit.log').read_text(errors='replace'); l=(base/f'logcat-fixed25-{tag}-admit.log').read_text(errors='replace')
 enc=[{'frames':int(a),'sec':float(b),'bytes':int(c)} for a,b,c in re.findall(r'stream 0 encoded (\d+) frames in ([\d.]+) s: [\d.]+ ms/frame .*?, (\d+) B/frame',s)]
 ren=[{'iter':int(a),'sec':float(b),'fps':float(c),'new':int(d)} for a,b,c,d in re.findall(r'render: (\d+) iterations in ([\d.]+) s \(([\d.]+)/s\), .*?(\d+) new-source',l)]
 net=[{'closed':int(a),'sec':float(b),'holes':int(c),'decoded':int(d)} for a,b,c,d in re.findall(r'nxwarp\[0\] net: (\d+) frames closed in ([\d.]+) s, (\d+) with a hole, .*?, (\d+) decoded so far',l)]
 e=mid(enc); r=mid(ren); n=mid(net)
 enc_rows=[{'window':i+1,'encoder_fps':x['frames']/x['sec'],'bytes_per_frame':x['bytes'],'payload_mbps':x['frames']*x['bytes']*8/x['sec']/1e6} for i,x in enumerate(e)]
 render_rows=[{'window':i+1,'app_renderloop_fps':x['iter']/x['sec'],'viewer_new_source_fps':x['new']/x['sec']} for i,x in enumerate(r)]
 net_rows=[{'window':i+1,'closed_fps':x['closed']/x['sec'],'holes':x['holes'],'decoded_total':x['decoded']} for i,x in enumerate(n)]
 def avg(rows,k):
  v=[x[k] for x in rows if k in x]; return statistics.mean(v) if v else None
 summary={'encoder_fps':avg(enc_rows,'encoder_fps'),'viewer_new_source_fps':avg(render_rows,'viewer_new_source_fps'),'app_renderloop_fps':avg(render_rows,'app_renderloop_fps'),'closed_fps':avg(net_rows,'closed_fps'),'payload_mbps':avg(enc_rows,'payload_mbps'),'bytes_per_frame':avg(enc_rows,'bytes_per_frame'),'holes_per_2s_window':avg(net_rows,'holes')}
 return {'tag':tag,'summary':summary,'encoder_windows':enc_rows,'render_windows':render_rows,'net_windows':net_rows,'source_counts':{'encoder':len(enc),'render':len(ren),'net':len(net)}}
res={x:parse(x) for x in ('raw','wire')}; (out/'results.json').write_text(json.dumps(res,indent=2)+'\n')
for tag in ('raw','wire'):
 lines=[]
 for fn in (f'server-fixed25-{tag}-admit.log',f'logcat-fixed25-{tag}-admit.log'):
  keep=[x for x in (base/fn).read_text(errors='replace').splitlines() if ('stream 0 encoded ' in x or 'render: ' in x and ('iterations in' in x or 'source->first' in x) or 'nxwarp[0] net:' in x or 'lossless ' in x and 'compressed' in x)]
  lines += [f'### {fn}'] + keep[5:-1]
 (out/f'{tag}-evidence.log').write_text('\n'.join(lines)+'\n')
