#!/usr/bin/env python3
"""Check native Pico raw readbacks: forced centre updates retain old periphery."""
import hashlib
import json
import sys
from pathlib import Path
import numpy as np
from PIL import Image
root = Path(sys.argv[1])
images = {name: np.fromfile(root / (name + '.rgba'), np.uint8).reshape(2176, 4352, 4)
          for name in ('full0', 'full19', 'centre19')}
y, x = np.indices((2176, 4352))
radius = np.maximum(abs(2 * ((x // 64) % 34) - 33), abs(2 * (y // 64) - 33))
centre = radius * 4 // 34 == 0
old, new, actual = (images[k] for k in ('full0', 'full19', 'centre19'))
assert np.array_equal(actual[centre], new[centre]), 'centre differs from fresh full render'
assert np.array_equal(actual[~centre], old[~centre]), 'periphery not retained exactly'
changed = np.any(new != old, axis=2)
assert np.any(changed & centre), 'centre test has no changing pixels'
assert np.any(changed & ~centre), 'periphery test has no changing pixels'
result = {'centre_matches_fresh': True, 'periphery_matches_initial': True,
          'centre_changed_pixels': int((changed & centre).sum()),
          'periphery_changed_pixels': int((changed & ~centre).sum()),
          'sha256': {k: hashlib.sha256(v.tobytes()).hexdigest() for k,v in images.items()}}
for name, image in images.items():
    Image.fromarray(image).save(root / (name + '.png'))
print(json.dumps(result, indent=2))
