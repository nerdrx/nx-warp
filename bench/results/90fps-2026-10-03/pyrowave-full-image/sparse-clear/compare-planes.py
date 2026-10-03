from pathlib import Path
import numpy as np
base=Path(__file__).parent
for plane in ('y','cb','cr'):
    expected=np.fromfile(base/f'reference-{plane}.raw',dtype=np.uint8)
    actual=np.fromfile(base/f'sparse-clear.frame000.{plane}.raw',dtype=np.uint8)
    print(plane,'n=',actual.size,'exact=',np.array_equal(expected,actual),
          'max_abs=',int(np.abs(actual.astype('i2')-expected.astype('i2')).max()),
          'mae=',float(np.abs(actual.astype('i2')-expected.astype('i2')).mean()))
