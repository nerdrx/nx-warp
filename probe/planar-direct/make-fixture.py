#!/usr/bin/env python3
"""Changing-pixel stereo pan fixture; independent all-PLANAR frames, no poses."""
import argparse, hashlib, json, math, os, pathlib, subprocess, tempfile, time
import numpy as np

p = argparse.ArgumentParser()
p.add_argument('--encoder', type=pathlib.Path, required=True)
p.add_argument('--out', type=pathlib.Path, required=True)
p.add_argument('--frames', type=int, default=120)
p.add_argument('--width', type=int, default=4352)
p.add_argument('--height', type=int, default=2176)
p.add_argument('--qp', type=int, default=24)
p.add_argument('--api-image', action='store_true',
                help='use nxvc-vkenc-api --image with the explicit GPU PLANAR opt-in')
a = p.parse_args()
if a.frames < 1 or a.width % 128 or a.height % 64:
    p.error('positive frame count and tile-aligned stereo geometry required')
a.out = a.out.resolve(); a.out.parent.mkdir(parents=True, exist_ok=True)
y, x = np.indices((a.height, a.width // 2), dtype=np.int32)
base = np.where(((x // 96) ^ (y // 96)) & 1, 70, 180).astype(np.uint8)
base[(x % 384 < 20) | (y % 384 < 20)] = 235
base[((x - a.width // 4)**2 + (y - a.height // 2)**2) < (a.height // 5)**2] = 35
# High-contrast edges on a plane; translation is synthetic, not a VR head-turn capture.
planes = [np.concatenate((base, np.roll(base, -18, axis=1)), axis=1)]
for phase in (0, 1):
    c = np.where(((x[::2, ::2] // 160 + phase) ^ (y[::2, ::2] // 128)) & 1, 110, 145).astype(np.uint8)
    planes.append(np.concatenate((c, np.roll(c, -9, axis=1)), axis=1))
manifest = dict(scope='synthetic changing-pixel plane pan, no pose/timewarp, independent PLANAR frames', width=a.width, height=a.height, frames=a.frames, qp=a.qp, source_frames=[], backend='nxvc-vkenc-api --image' if a.api_image else 'nxvc-vkenc')
with tempfile.TemporaryDirectory(prefix='nx-planar-pan-') as temp:
    fifo = pathlib.Path(temp) / 'source.yuv'; os.mkfifo(fifo)
    cmd = [str(a.encoder.resolve()), '--in', str(fifo), '--w', str(a.width), '--h', str(a.height), '--pix', 'yuv420p', '--eyes', '2', '--frames', str(a.frames), '--qp', str(a.qp), '--inter', '--atlas', '--entropy', 'lite' if a.api_image else 'lite-fixed', '--out', str(a.out)]
    if not a.api_image:
        cmd.insert(cmd.index('--entropy'), '--planar-prefer')
    else:
        cmd[cmd.index('--pix'):cmd.index('--pix') + 2] = []
        cmd.insert(1, '--image')
    env = dict(os.environ, NXVC_PLANAR_FORCE='1', NXVC_PLANAR_CONFIG='2,0')
    if a.api_image:
        env['NXVC_ENC_PLANAR_GPU_FLAT'] = '1'
    with a.out.with_suffix('.encode.log').open('w') as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
        # A dead encoder must not leave the writer waiting forever for its reader.
        import errno
        while True:
            if proc.poll() is not None: raise RuntimeError('encoder exited before FIFO open')
            try: fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK); break
            except OSError as e:
                if e.errno != errno.ENXIO: raise
                time.sleep(.02)
        os.set_blocking(fd, True)
        start = time.monotonic()
        try:
            with os.fdopen(fd, 'wb') as f:
                for frame in range(a.frames):
                    dx = 6 * frame
                    # Shift the vertical phase after each horizontal wrap so longer
                    # stress sequences do not cycle after width/gcd(width, 6).
                    dy = 2 * frame + 2 * (frame // (a.width // math.gcd(a.width, 6)))
                    digest = hashlib.sha256()
                    for i, plane in enumerate(planes):
                        scale = 1 if i == 0 else 2
                        data = np.roll(plane, (dy // scale, dx // scale), axis=(0, 1)).tobytes()
                        digest.update(data); f.write(data)
                    manifest['source_frames'].append(dict(frame=frame, dx=dx, dy=dy, sha256=digest.hexdigest()))
        finally:
            rc = proc.wait(timeout=300)
        if rc: raise RuntimeError(f'encoder failed: {rc}')
        manifest['generation_and_encoding_seconds'] = time.monotonic() - start
manifest['distinct_source_frames'] = len({f['sha256'] for f in manifest['source_frames']})
manifest['stream_sha256'] = hashlib.sha256(a.out.read_bytes()).hexdigest()
manifest['stream_bytes'] = a.out.stat().st_size
manifest['command'] = cmd
if manifest['distinct_source_frames'] != a.frames: raise RuntimeError('source frame repetition')
a.out.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(a.out, manifest['stream_bytes'], 'bytes;', a.frames, 'distinct frames')
