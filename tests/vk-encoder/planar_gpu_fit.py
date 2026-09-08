#!/usr/bin/env python3
"""Pixel regression for the opt-in GPU R2/coarse PLANAR fit."""
import argparse, os, pathlib, subprocess, tempfile
import numpy as np

W, H = 256, 192
def frame(flat=False):
    y = np.empty((H, W), np.uint8); u = np.empty((H//2, W//2), np.uint8); v = np.empty_like(u)
    if flat:
        y.fill(128); u.fill(128); v.fill(128)
    else:
        y[:] = np.where(np.arange(W) % 64 < 32, 50, 200)
        u[:] = np.where(np.arange(W//2) % 32 < 16, 100, 150)
        v[:] = np.where(np.arange(W//2) % 32 < 16, 110, 140)
    return y.tobytes() + u.tobytes() + v.tobytes()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--encoder', required=True, type=pathlib.Path)
    p.add_argument('--decoder', required=True, type=pathlib.Path)
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='nx-planar-gpu-fit-') as td:
        td = pathlib.Path(td); src, stream, decoded = td/'in.yuv', td/'out.nxv', td/'out.yuv'
        src.write_bytes(frame(True) + frame(False))
        for eyes in (1, 2):
            env = dict(os.environ, NXVC_ENC_PLANAR_GPU_FLAT='1', NXVC_PLANAR_FORCE='1', NXVC_PLANAR_CONFIG='2,0')
            enc = [str(a.encoder.resolve()), '--in', str(src), '--w', str(W), '--h', str(H),
                   '--pix', 'yuv420p', '--eyes', str(eyes), '--frames', '2', '--qp', '40', '--inter', '--atlas',
                   '--planar-prefer', '--entropy', 'lite-fixed', '--out', str(stream)]
            subprocess.run(enc, env=env, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run([str(a.decoder.resolve()), '--in', str(stream), '--out', str(decoded),
                            '--pix', 'yuv420p'], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            raw = np.fromfile(decoded, np.uint8); fs = W*H + 2*(W//2)*(H//2)
            assert raw.size == 2*fs, (raw.size, fs)
            got = raw.reshape(2, fs)
            expected = np.frombuffer(frame(False), np.uint8)
            # Check every sample: averaging entire planes could conceal bad slopes
            # or swapped region labels. QP40's DC step is 161/16 samples.
            assert np.max(np.abs(got[0].astype(int) - 128)) == 0
            assert np.max(np.abs(got[1].astype(int) - expected.astype(int))) <= 6
            spans = [(0, W*H, W, 64), (W*H, W*H*5//4, W//2, 32),
                     (W*H*5//4, fs, W//2, 32)]
            for lo, hi, width, tile in spans:
                arr = got[1, lo:hi].reshape(-1, width)
                for region in (False, True):
                    selected = arr[:, (np.arange(width) % tile < tile//2) == region]
                    assert np.ptp(selected) == 0, "nonzero slope or inconsistent tile fit"
        print('planar GPU fit pixel regression: PASS')
if __name__ == '__main__': main()
