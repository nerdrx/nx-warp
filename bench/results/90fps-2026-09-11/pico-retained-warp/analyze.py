"""Requested timeline continuity, not physical image/pose latency."""
import re,json,sys,numpy as np
rows=[]
for line in open(sys.argv[1]):
 m=re.search(r'warp timeline: display (\d+) frame (\d+) compositor (\d+) anchor (\d+) span (\d+) step ([\d.]+) field (true|false) cache (true|false)',line)
 if not m:continue
 display,frame,comp,anchor,span=map(int,m.group(1,2,3,4,5));step=float(m[6]);field=m[7]=='true'
 rows.append(dict(display=display,frame=frame,represented=(anchor+span*step if field else comp),active=field and step>0))
rows=rows[300:] # Exclude first 300 traced presentations (startup/warm-up).
pairs=[(a,b) for a,b in zip(rows,rows[1:]) if 0<b['display']-a['display']<100_000_000]
d=np.array([(b['represented']-a['represented'])/1e6 for a,b in pairs]);expected=np.array([(b['display']-a['display'])/1e6 for a,b in pairs]);changes=np.array([a['frame']!=b['frame'] for a,b in pairs]);toggle=sum(a['active']!=b['active'] for a,b in pairs)
print(json.dumps({'samples':len(rows),'valid_pairs':len(pairs),'active_fraction':np.mean([r['active'] for r in rows]),'activity_toggles':toggle,'backsteps_over_1ms':int((d < -1).sum()),'stalls_under_1ms':int((abs(d)<1).sum()),'timeline_step_p5_p50_p95_ms':np.percentile(d,[5,50,95]).tolist(),'step_minus_display_step_rms_ms':float(np.sqrt(np.mean((d-expected)**2))),'source_switches':int(changes.sum()),'source_switch_backsteps':int(((d<-1)&changes).sum())},indent=2))
