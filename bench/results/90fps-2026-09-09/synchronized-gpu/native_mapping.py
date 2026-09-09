"""Independent nearest-retained-sample oracle for compact centre geometry."""
import json
from pathlib import Path
import numpy as np
axis=np.concatenate([np.arange(1,832,4),np.arange(832,1344),np.arange(1345,2176,4)])
assert len(axis)==928 and np.all(np.diff(axis)>0)
def nearest(n):
 right=np.searchsorted(axis,n,side='left').clip(0,len(axis)-1)
 left=(right-1).clip(0,len(axis)-1)
 return np.where(np.abs(axis[right]-n)<=np.abs(axis[left]-n),right,left)
assert np.array_equal(nearest(axis),np.arange(928))
records=[]
for dx in [-64,-5,-3,-1,0,1,3,5,64]:
 native=axis-dx;valid=(native>=0)&(native<2176)
 mapped=nearest(native)
 assert np.all((mapped>=0)&(mapped<928))
 if dx==0:assert np.array_equal(mapped,np.arange(928))
 records.append({'dx_native':dx,'invalid_columns':int((~valid).sum()),'max_nearest_error_native':int(np.abs(axis[mapped[valid]]-native[valid]).max())})
if __name__=='__main__':
 Path(__file__).with_name('mapping-checks.json').write_text(json.dumps({'exact_retained_roundtrip':True,'nearest_tie':'upper index','translation_checks':records},indent=2)+'\n')
 print('Native compact mapping: exact retained-sample roundtrip and signed-shift checks passed.')
