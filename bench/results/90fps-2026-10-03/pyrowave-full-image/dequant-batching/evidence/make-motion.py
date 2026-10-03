from pathlib import Path
import hashlib, struct, subprocess
import numpy as np
out=Path(__file__).parent
srcpath=Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003/motion-visual/private-dark-native-stereo-420.yuv')
w,h=4352,2176
cw,ch=w//2,h//2
raw=np.fromfile(srcpath,dtype=np.uint8)
assert raw.size==w*h+2*cw*ch
planes=(raw[:w*h].reshape(h,w),raw[w*h:w*h+cw*ch].reshape(ch,cw),raw[w*h+cw*ch:].reshape(ch,cw))
shifts=[(0,0),(2,2),(4,2),(6,0),(4,-2),(2,-2)]
frames=[]
enc=Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003/motion-visual/encoder-420')
for i,(dx,dy) in enumerate(shifts):
    # np.roll intentionally wraps pixels at image borders.
    moved=[np.roll(p,shift=(dy if k==0 else dy//2,dx if k==0 else dx//2),axis=(0,1)) for k,p in enumerate(planes)]
    rawframe=np.concatenate([p.reshape(-1) for p in moved]).astype(np.uint8)
    f=out/'frames'/f'frame-{i:02}.yuv'; rawframe.tofile(f)
    packetfile=out/'frames'/f'frame-{i:02}.pyrowave'
    log=out/'frames'/f'encode-{i:02}.log'
    with log.open('w') as lf: subprocess.run([str(enc),str(f),str(w),str(h),'420',str(packetfile)],stdout=lf,stderr=subprocess.STDOUT,check=True)
    b=packetfile.read_bytes()
    assert b[:8]==b'PYROHAAR' and len(b)>40
    frames.append(b)
assert all(b[:40]==frames[0][:40] for b in frames)
stream=frames[0][:40]+b''.join(b[40:] for b in frames)
(out/'motion-6.pyrowave').write_bytes(stream)
for i,(dx,dy) in enumerate(shifts): print(f'{i}: dx={dx},dy={dy}, chroma=({dx//2},{dy//2}), bytes={len(frames[i])},sha256={hashlib.sha256(frames[i]).hexdigest()}')
print(f'stream_bytes={len(stream)} sha256={hashlib.sha256(stream).hexdigest()}')
