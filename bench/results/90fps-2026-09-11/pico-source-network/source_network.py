"""Parse event-triggered controller snapshots; these are not complete loss totals."""
from pathlib import Path
import re,json
r=Path(__file__).resolve().parent;live=r.parent/'motion-live';out={}
for mode in ['60','off','off-repeat']:
 s=(live/f'pico-res100-source{mode}-server.log').read_text();events=[]
 for m in re.finditer(r'Automatic bitrate: (.*?), ([\d.]+) -> ([\d.]+) Mbit/s \(p90 utilisation ([\d.]+), (\d+) lost, (\d+) late over (\d+) frames\)',s):
  events.append(dict(reason=m[1],from_mbps=float(m[2]),to_mbps=float(m[3]),p90_utilisation=float(m[4]),lost=int(m[5]),late=int(m[6]),frames=int(m[7])))
 out[mode]=events
(r/'source-network.json').write_text(json.dumps(out,indent=2));print({k:len(v) for k,v in out.items()})
