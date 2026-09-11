from pathlib import Path
import re,json,numpy as np
r=Path(__file__).resolve().parent;live=r.parent/'motion-live';out={}
for mode in ['60','off','off-repeat']:
 s=(live/f'pico-res100-source{mode}-client.log').read_text();frames=re.findall(r'render: (\d+) iterations in ([\d.]+) s \(([\d.]+)/s\), (\d+) submitted a layer, (\d+) new-source',s)[2:]
 def mean(pattern):
  a=[float(x) for x in re.findall(pattern,s)[2:]];return float(np.mean(a)) if a else None
 out[mode]={'steady_windows':len(frames),'render_per_s':float(np.mean([float(x[2]) for x in frames])),'fresh_per_s':float(np.mean([int(x[4])/float(x[1]) for x in frames])),'gpu_pass_ms':mean(r'own GPU pass ([\d.]+)'),'source_offset_ms':mean(r'source display-time offset ([\d.-]+)'),'decode_ms':mean(r'wire [\d.]+ \| queue [\d.]+ \| decode ([\d.]+)'),'decode_to_selection_ms':mean(r'decode->selection ([\d.]+)'),'selection_to_predicted_ms':mean(r'selection->predicted ([\d.]+)'),'late_mean':mean(r'overrun \d+ late (\d+)')}
(r/'source-rate-summary.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
