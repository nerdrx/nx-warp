#!/usr/bin/env bash
set -euo pipefail
BIN=${1:?usage: $0 /path/to/build/bin}
OUT=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
python3 - "$OUT/fixture.yuv" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); w=h=2688; pw=2*w
y=bytearray(pw*h); uv=bytearray(pw*h//2)
for yy in range(h):
  for xx in range(pw):
    e,x=divmod(xx,w); y[yy*pw+xx]=(x*37+yy*19+e*73)&255
    uv[(yy//2)*pw+xx]=(x*11+yy*7+e*101)&255
p.write_bytes(y+uv)
PY
NXVC_PLANAR_ROUND=1 NXVC_PLANAR_COLOUR=1 NXVC_PLANAR_WIDE_RING=1 NXVC_PLANAR_LARGE_CENTRE=1 "$BIN/nxvc-vkenc-api" --in "$OUT/fixture.yuv" --w 5376 --h 2688 --eyes 2 --frames 1 --qp 24 --inter --intra-period 1 --planar-gpu-centre --centre-quarter --centre-graduated --out "$OUT/large.nxv"
"$BIN/nxvc-vkdec" --in "$OUT/large.nxv" --out "$OUT/native.yuv" --nv12 --independent-tiles
"$BIN/nxvc-vkdec" --in "$OUT/large.nxv" --out "$OUT/compact-default.yuv" --nv12 --compact-centre --independent-tiles
"$BIN/nxvc-vkdec" --in "$OUT/large.nxv" --out "$OUT/compact-large.yuv" --nv12 --compact-large-centre --independent-tiles
NXVC_VKD_PLANAR_FLAT=1 "$BIN/nxvc-vkdec" --in "$OUT/large.nxv" --out "$OUT/compact-large-flat64.yuv" --nv12 --compact-large-centre --compact-flat64 --independent-tiles
cmp "$OUT/compact-large.yuv" "$OUT/compact-large-flat64.yuv"
python3 - "$OUT" <<'PY'
from pathlib import Path
import sys, numpy as np
p=Path(sys.argv[1]); nw,nh,cw,ch=5376,2688,2880,1440
n=np.fromfile(p/'native.yuv',dtype=np.uint8); c=np.fromfile(p/'compact-large.yuv',dtype=np.uint8)
lx=np.r_[np.arange(1,832,4),np.arange(832,1856),np.arange(1857,2688,4)]
cx=np.r_[np.arange(1,416,4),np.arange(416,928),np.arange(929,1344,4)]
assert len(lx)==1440 and len(cx)==720
packed_lx=np.arange(1440); packed_cx=np.arange(720)
ny=n[:nw*nh].reshape(nh,nw); cy=c[:cw*ch].reshape(ch,cw)
for e in range(2): assert np.array_equal(cy[:,e*1440:(e+1)*1440],ny[np.ix_(lx,e*2688+lx)])
nu=n[nw*nh:].reshape(nh//2,nw//2,2); cu=c[cw*ch:].reshape(ch//2,cw//2,2)
for e in range(2): assert np.array_equal(cu[:,e*720:(e+1)*720],nu[np.ix_(cx,e*1344+cx)])
print('large compact full-axis luma+NV12 exactness: PASS')
PY
