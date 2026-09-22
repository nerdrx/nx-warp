from pathlib import Path
import json,re,sys
p=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(exist_ok=True)
tags=sys.argv[3:];results=[]
for tag in tags:
 s=(p/f'logcat-{tag}.log').read_text();server=(p/f'server-{tag}.log').read_text()
 w=[list(map(float,x)) for x in re.findall(r'(\d+) iterations in ([\d.]+) s .*?, (\d+) submitted a layer, (\d+) new-source',s)]
 lat=[list(map(float,x)) for x in re.findall(r'wire ([\d.]+) \| queue ([\d.]+) \| decode ([\d.]+) \| decode->selection ([\d.]+) \| selection->predicted ([\d.]+) \| sum [-\d.]+ over (\d+) selected',s) if int(x[-1])>10]
 net=[list(map(float,x)) for x in re.findall(r'net: (\d+) frames closed in ([\d.]+) s, (\d+) with a hole',s)]
 lz=re.findall(r'LZ4 (\d+) compressed / (\d+) raw units, (\d+) wire / (\d+) unpacked bytes, ([\d.]+) ms total decompression',s)
 r=dict(tag=tag,render_windows=len(w),fresh_after_first=sum(x[3] for x in w[1:])/sum(x[1] for x in w[1:]),receive_to_predicted_ms=sum(sum(x[:5])*x[5] for x in lat)/sum(x[5] for x in lat),closed_units=sum(x[0] for x in net),incomplete_units=sum(x[2] for x in net))
 if lz:
  a,b,c,d,e=map(float,lz[-1]);r.update(compressed_units=a,raw_units=b,payload_saved_percent=100*(1-c/d),decompress_mean_ms=e/a)
 results.append(r)
 (out/f'{tag}.log').write_text('\n'.join(x for x in s.splitlines() if ('iterations in ' in x or 'net: ' in x or ' | queue ' in x or 'LZ4 ' in x or 'partial direct recovery' in x))+'\n')
 (out/f'{tag}-server.log').write_text('\n'.join(x for x in server.splitlines() if ('encoded ' in x and 'frames in ' in x) or ('NX direct RGB' in x))+'\n')
(out/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
