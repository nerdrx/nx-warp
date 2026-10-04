#!/usr/bin/env python3
"""Generate Basis ASTC 5x5 weight interpolation pseudoinverses for 6x6 and 8x8."""
from pathlib import Path
import numpy as np
OUT=Path(__file__).with_name('inverse.glsl')
def basis_samples(block,grid=5):
    s=(1024+block//2)//(block-1); rows=[]
    for ty in range(block):
      for tx in range(block):
        gx=(s*tx*(grid-1)+32)>>6; gy=(s*ty*(grid-1)+32)>>6
        jx,jy,fx,fy=gx>>4,gy>>4,gx&15,gy&15
        w11=(fx*fy+8)>>4; w10, w01=fy-w11, fx-w11; w00=16-fx-fy+w11
        row=np.zeros(grid*grid,dtype=np.float64)
        for dy,dx,w in ((0,0,w00),(0,1,w01),(1,0,w10),(1,1,w11)):
          if w: row[(jy+dy)*grid+jx+dx]+=w/16.0
        assert abs(row.sum()-1)<1e-12; rows.append(row)
    return np.array(rows)
with OUT.open('w') as f:
  f.write('// Generated with Basis compute_upsample_weights expressions; row-major [gridWeight][texel].\n')
  for n in (6,8):
    a=basis_samples(n); b=np.linalg.pinv(a); assert np.linalg.matrix_rank(a)==25
    assert np.max(np.abs(b@a-np.eye(25)))<1e-12
    vals=b.ravel(); name=f'astc5x5Inverse{n}'
    f.write(f'const float {name}[{len(vals)}] = float[{len(vals)}](\n')
    for i in range(0,len(vals),8):
      f.write('    '+', '.join(f'{v:.9g}' for v in vals[i:i+8]))
      f.write(',\n' if i+8<len(vals) else '\n')
    f.write(');\n')
