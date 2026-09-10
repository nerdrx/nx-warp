import json,re,sys
from pathlib import Path
root=Path(sys.argv[1]);out=[]
for label in ('base-a','candidate-a','candidate-b','base-b'):
 p=root/f'compact-flat64-long-{label}-client.log';s=p.read_text()
 trans=[tuple(map(int,m)) for m in re.findall(r'selected source transitions forward (\d+) backward (\d+) repeat (\d+), greatest forward gap (\d+)',s)]
 dec=[(int(a),float(b)) for a,b in re.findall(r'nxwarp\[0\]: (\d+) frames in ([\d.]+) s:',s)]
 out.append(dict(run=label,render_windows=len(trans),forward=sum(x[0] for x in trans),backward=sum(x[1] for x in trans),repeat=sum(x[2] for x in trans),greatest_forward_gap=max(x[3] for x in trans),older_available=sum(map(int,re.findall(r'selected older than available (\d+)',s))),decoded_frames=sum(x[0] for x in dec),decode_reported_seconds=sum(x[1] for x in dec),decode_fps=sum(x[0] for x in dec)/sum(x[1] for x in dec)))
print(json.dumps(out,indent=2))
