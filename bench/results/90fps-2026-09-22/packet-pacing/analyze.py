from pathlib import Path
import re,json,sys
p=Path(__file__).resolve().parent
def udp(s):
 a=[x.split()[1:] for x in s.splitlines() if x.startswith('Udp:')];return dict(zip(a[0],map(int,a[1])))
results={}
for tag in (sys.argv[1:] or ['burst500','paced500','paced200','paced300','burst300','burst200']):
 s=(p/f'logcat-{tag}.log').read_text();w=[list(map(float,x)) for x in re.findall(r'(\d+) iterations in ([\d.]+) s .*?, (\d+) submitted a layer, (\d+) new-source',s)]
 lat=[list(map(float,x)) for x in re.findall(r'wire ([\d.]+) \| queue ([\d.]+) \| decode ([\d.]+) \| decode->selection ([\d.]+) \| selection->predicted ([\d.]+) \| sum [-\d.]+ over (\d+) selected',s) if int(x[-1])>10]
 net=[list(map(float,x)) for x in re.findall(r'net: (\d+) frames closed in ([\d.]+) s, (\d+) with a hole',s)]
 r={'render_windows':len(w),'fresh_after_first':sum(x[3] for x in w[1:])/sum(x[1] for x in w[1:]) if len(w)>1 else None,'receive_to_predicted_ms':sum(sum(x[:5])*x[5] for x in lat)/sum(x[5] for x in lat) if lat else None,'closed_frames':sum(x[0] for x in net),'holes':sum(x[2] for x in net)}
 for side in ['host','pico']:
  a,b=[udp((p/f'{side}-snmp-{phase}-{tag}.txt').read_text()) for phase in ['before','after']];r[side+'_udp_delta']={k:b[k]-v for k,v in a.items()}
 results[tag]=r
print(json.dumps(results,indent=2))
