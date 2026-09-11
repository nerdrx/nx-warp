from pathlib import Path
import re,json,subprocess,numpy as np
r=Path(__file__).resolve().parent;live=r.parent/'motion-live';results={}
for cap in ['5000','10000']:
 p=live/('pico-res100-jit'+cap+'-client.log');s=p.read_text()
 v=json.loads(subprocess.check_output(['python3',str(live/'analyze_live.py'),str(p)]))
 patterns={'decode_ms':r'wire [\d.]+ \| queue [\d.]+ \| decode ([\d.]+)', 'decode_to_selection_ms':r'decode->selection ([\d.]+)', 'selection_to_predicted_ms':r'selection->predicted ([\d.]+)', 'sleep_ms':r'slept ([\d.]+) ms mean','overrun':r'misses: \d+ overrun (\d+)','late':r'overrun \d+ late (\d+)'}
 v['post_startup_window_means']={k:float(np.mean([float(x) for x in re.findall(pattern,s)[2:]])) for k,pattern in patterns.items()}
 results[cap]=v
(r/'res100-summary.json').write_text(json.dumps(results,indent=2));print(json.dumps(results,indent=2))
