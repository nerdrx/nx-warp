import json,re,sys
from pathlib import Path

def seconds(t):
 h,m,s=map(float,t.split(':'));return h*3600+m*60+s
for arg in sys.argv[1:]:
 text=Path(arg).read_text(errors='replace');rows=[]
 for line in text.splitlines():
  m=re.search(r'render: (\d+) iterations in ([0-9.]+) s .*? (\d+) new-source',line)
  t=re.search(r'\d\d-\d\d (\d\d:\d\d:\d\d\.\d+)',line)
  if m and t: rows.append({'t':seconds(t[1]),'iterations':int(m[1]),'seconds':float(m[2]),'fresh':int(m[3])})
 start=rows[0]['t'] if rows else 0;rows=[r for r in rows if (r['t']-start)%86400>=10]
 gaps=[(b['t']-a['t'])%86400 for a,b in zip(rows,rows[1:])]
 reported=sum(r['seconds'] for r in rows);elapsed=((rows[-1]['t']-rows[0]['t'])%86400+rows[0]['seconds']) if rows else 0
 print(json.dumps({'file':arg,'stopping_events':len(re.findall(r'=> XR_SESSION_STATE_STOPPING',text)),'post_warm_windows':len(rows),'reported_seconds':reported,'covered_wall_seconds':elapsed,'max_window_gap_seconds':max(gaps,default=0),'fresh_per_reported_second':sum(r['fresh'] for r in rows)/reported if reported else None,'fresh_per_covered_wall_second':sum(r['fresh'] for r in rows)/elapsed if elapsed else None,'scope':'Logged render-window coverage after10s. Rounded reporting durations; not panel/photon timing. Includes gaps between windows.'}))
