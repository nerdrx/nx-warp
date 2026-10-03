from pathlib import Path
import numpy as np, subprocess, struct, shutil
root=Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-haar-20261003')
base=root/'private-dark-native-stereo-444.yuv'
out=root/'motion-visual'; w,h=4352,2176; n=w*h
src=np.fromfile(base,dtype=np.uint8).reshape(3,h,w)
# Six small, reversible translations; clamp at borders to avoid wrap seams.
shifts=[(0,0),(1,0),(2,1),(1,2),(0,1),(-1,0)]
for idx,(dx,dy) in enumerate(shifts):
    planes=[]
    for pl in src:
        padded=np.pad(pl,((3,3),(3,3)),mode='edge')
        planes.append(padded[3+dy:3+dy+h,3+dx:3+dx+w])
    raw=np.stack(planes).astype(np.uint8)
    frame=out/f'private-frame-{idx:02}.yuv'; raw.tofile(frame)
    subprocess.run([str(root/'pyrowave-haar-host-encoder'),str(frame),str(w),str(h),'444',str(out/f'haar-frame-{idx:02}.pyrowave')],check=True)
    # CDF encoder was built from its saved source tree in prior task.
    cdf=Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003/pyrowave-host-encoder')
    if not cdf.exists(): cdf=Path('/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003/pyrowave-haar-host-encoder')
    if not cdf.exists(): raise SystemExit('CDF host encoder not found')
    subprocess.run([str(cdf),str(frame),str(w),str(h),'444',str(out/f'cdf-frame-{idx:02}.pyrowave')],check=True)
    frame.unlink()
# A packet stream is one common header followed by each independently encoded packet record.
for kind in ('haar','cdf'):
    chunks=[(out/f'{kind}-frame-{i:02}.pyrowave').read_bytes() for i in range(len(shifts))]
    if any(c[:8]!=chunks[0][:8] or c[8:40]!=chunks[0][8:40] for c in chunks): raise SystemExit('header mismatch')
    stream=out/f'{kind}-translated-6.pyrowave'
    stream.write_bytes(chunks[0][:40]+b''.join(c[40:] for c in chunks))
    print(kind,stream.stat().st_size)
