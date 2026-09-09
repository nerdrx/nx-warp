"""Independent checks for the CPU model; no production decoder tests."""
import importlib.util,json
from pathlib import Path
import numpy as np
p=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('model',p/'prototype.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
metrics=json.loads((p/'metrics.json').read_text());final=np.load(p/'final_review.npz')
for name in metrics['summaries']:
 assert np.array_equal(final[name][:,192:320,192:320],final['truth'][:,192:320,192:320])
budgets=[r['refresh_eye_pixels'] for r in metrics['summaries'].values()]
assert len(set(budgets))==1,budgets
assert metrics['loss_reset_stress']['bounded']
# Read-only quality baseline: new half-width/height guide plus exact centre.
errors=[]
for f in range(1,m.FRAMES):
 truth,_=m.source(f);out=m.up(m.guide(truth));out[:,m.CENTRE]=truth[:,m.CENTRE]
 errors.append(float((np.abs(out-truth)*255)[:,~m.CENTRE].mean()))
# An odd checker translation is invisible in a 2x2 guide. Ensure the cheap
# eye actually falls back, rather than retaining a confidently wrong phase.
def checker(f):
 a=((m.X+m.Y+f)%2).astype(np.float32)
 return np.stack([a,a]),np.zeros((2,m.H,m.W),bool)
m.source=checker
rows,last=m.run('alternating_estimated')
f=m.FRAMES-1;cheap_eye=1-f%2
assert np.all(last['output'][cheap_eye][~m.CENTRE]==.5)
assert np.array_equal(last['output'][f%2],last['truth'][f%2])
assert all(r['confidence_gap']==0 for r in rows)
result={'all_final_centres_exact':True,'equal_detail_budget':True,
'loss_reset_age_bound':True,'checker_cheap_eye_guide_fallback':True,
'half_guide_baseline_periphery_mae':float(np.mean(errors)),
'limits':'CPU quality assertions, not Pico/decoder validity or comfort tests.'}
(p/'review-checks.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
