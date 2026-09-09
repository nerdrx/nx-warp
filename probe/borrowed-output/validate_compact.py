#!/usr/bin/env python3
"""Compare compact NV12 GPU readback to exact representative samples of CPU YUV420."""
import argparse, hashlib, json
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument('reference_yuv');p.add_argument('compact_nv12');a=p.parse_args()
ref=Path(a.reference_yuv).read_bytes();got=Path(a.compact_nv12).read_bytes()
w,h=4352,2176;frame=w*h*3//2
assert len(ref)%frame==0

def axis(n,core):
    start=(n-core)//2
    return np.concatenate((np.arange(1,start,4),np.arange(start,start+core),np.arange(start+core+1,n,4)))

def select(plane,n,core):
    idx=axis(n,core);xx=np.concatenate((idx,idx+n))
    return plane[np.ix_(idx,xx)]

expected=bytearray()
for offset in range(0,len(ref),frame):
    f=np.frombuffer(ref[offset:offset+frame],dtype=np.uint8);n=w*h
    y=f[:n].reshape(h,w);u=f[n:n+n//4].reshape(h//2,w//2);v=f[n+n//4:].reshape(h//2,w//2)
    sy=select(y,h,512);su=select(u,h//2,256);sv=select(v,h//2,256)
    expected.extend(sy.tobytes());expected.extend(np.stack((su,sv),axis=-1).tobytes())
result={'frames':len(ref)//frame,'width':int(sy.shape[1]),'height':int(sy.shape[0]),'bytes':len(got),'expected_bytes':len(expected),'exact':got==expected,'sha256':hashlib.sha256(got).hexdigest(),'expected_sha256':hashlib.sha256(expected).hexdigest()}
print(json.dumps(result,indent=2))
raise SystemExit(0 if result['exact'] else 1)
