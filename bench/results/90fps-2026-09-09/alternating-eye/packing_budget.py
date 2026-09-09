"""Exact output-sample accounting; no GPU or latency measurement."""
import json
from pathlib import Path
import numpy as np
# Mirror the compact decoder's independent per-axis sampling steps.
axis=np.concatenate([np.arange(t*64,(t+1)*64,1) if 13<=t<21 else np.arange(t*64+1,(t+1)*64,4) for t in range(34)])
assert len(axis)==928
centre=(axis>=832)&(axis<1344)
protected=centre[:,None]&centre[None,:]
# Idealized guide thins every other retained sample in both axes, keeping centre.
i=np.arange(928);cheap=protected|((i[:,None]%2==0)&(i[None,:]%2==0))
full=928**2;lo=int(cheap.sum());assert lo==411904
result={'native_eye_samples':2176**2,'current_packed_eye_samples':full,
'native_centre_samples':512**2,'hypothetical_cheap_eye_samples':lo,
'alternating_pair_mean_samples_per_eye':(full+lo)/2,
'ideal_output_sample_reduction_fraction':1-(full+lo)/(2*full),
'native_half_resolution_guide_samples':1088**2,
'native_half_guide_vs_current_packed_ratio':1088**2/full,
'limits':'Integer luma sample accounting only. Half resolution here means half width AND height of current packed peripheral samples, not native resolution. Excludes entropy, transforms, per-tile dispatches, guide/history reads, warp, metadata, chroma alignment, and unchanged final display writes. Not measured GPU saving or latency.'}
Path(__file__).with_name('packing-budget.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
