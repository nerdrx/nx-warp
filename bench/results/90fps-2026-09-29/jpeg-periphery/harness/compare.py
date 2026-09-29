"""Offline NXVC-centre/JPEG-periphery experiment. Private image data stays private."""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import struct

import numpy as np
from PIL import Image, features

SIDE = 2176
CORE = 128
OUTER = 384
HEADER = struct.Struct('<4s8I')
HASHES = {
    'forest': '033da4919b1652854afa1369e4c3557880436054897340bc93418b4286bfae5a',
    'dark': '9544039448fabbe2afe97356d09d3dfcaed5c202fe60506985e6d67a4c069805',
}


def packet_parts(data):
    if len(data) < HEADER.size:
        raise ValueError('short header')
    magic, version, width, height, core, outer, nx_n, jpg_n, reserved = HEADER.unpack_from(data)
    if (magic, version, width, height, core, outer, reserved) != (
            b'NXJP', 1, 2*SIDE, SIDE, CORE, OUTER, 0):
        raise ValueError('unsupported experiment layout')
    if not nx_n or not jpg_n or len(data) != HEADER.size + nx_n + jpg_n:
        raise ValueError('invalid packet lengths')
    return data[HEADER.size:HEADER.size+nx_n], data[HEADER.size+nx_n:]


def psnr(reference, decoded, mask):
    delta = decoded.astype(np.float32) - reference
    mse = np.mean(delta[mask]**2, dtype=np.float64)
    return 999.0 if mse == 0 else 10*math.log10(255**2 / mse)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--forest', type=Path, required=True)
    p.add_argument('--dark', type=Path, required=True)
    p.add_argument('--nx-decoded', type=Path, required=True)
    p.add_argument('--centre-envelopes', type=Path, required=True)
    p.add_argument('--centre-decoded', type=Path, required=True)
    p.add_argument('--nx-summary', type=Path, required=True)
    p.add_argument('--private', type=Path, required=True)
    p.add_argument('--results', type=Path, required=True)
    args = p.parse_args()
    args.private.mkdir(parents=True, exist_ok=True)
    args.results.mkdir(parents=True, exist_ok=True)
    baseline = json.loads(args.nx_summary.read_text())['fixtures']
    yy, xx = np.mgrid[:SIDE, :SIDE]
    radius = np.hypot(xx-(SIDE/2-.5), yy-(SIDE/2-.5))
    t = np.clip((radius-CORE)/(OUTER-CORE), 0, 1)
    weight = (1 - t*t*t*(t*(6*t-15)+10)).astype(np.float32)[..., None]
    core, periphery = radius <= CORE, radius >= OUTER
    active = radius < OUTER
    rows, jobs, fixtures, validation = [], [], [], []
    for scene in ('forest', 'dark'):
        raw_source = getattr(args, scene).read_bytes()
        assert hashlib.sha256(raw_source).hexdigest() == HASHES[scene]
        source = np.frombuffer(raw_source, np.uint8).reshape(2160, 2160, 4)[..., :3]
        for shift in (0, 8, 16):
            name = f'{scene}-s{shift}'
            # Production fixture shifts before foveation. Map output pixel
            # centres to input using the shader's floored coordinate convention.
            # This RGB reference does not apply its lossy NV12 colour prefilter.
            shifted = source[:, np.clip(np.arange(2160)+shift, 0, 2159)]
            coordinates = np.clip(np.floor((np.arange(SIDE)+.5)*2160/SIDE-.5).astype(int), 0, 2159)
            original = shifted[coordinates[:, None], coordinates[None, :]]
            nx_stereo = np.array(Image.open(args.nx_decoded / (name+'.ppm')))
            centre_stereo = np.array(Image.open(args.centre_decoded / (name+'.ppm')))
            assert nx_stereo.shape == centre_stereo.shape == (SIDE, 2*SIDE, 3)
            assert np.array_equal(nx_stereo[:, :SIDE], nx_stereo[:, SIDE:])
            nx = nx_stereo[:, :SIDE]
            for eye in range(2):
                assert np.array_equal(centre_stereo[:, eye*SIDE:(eye+1)*SIDE][active], nx[active])
            centre = centre_stereo[:, :SIDE]
            nx_payload = (args.centre_envelopes / (name+'.detail.bin')).read_bytes()
            safety_n = baseline[name]['safety_wire_bytes']
            nx_total = baseline[name]['selected_bytes'] + safety_n
            nx_psnr = psnr(original, nx, periphery)
            fixtures.append(dict(fixture=name, source_sha256=HASHES[scene],
                                 source_shift=shift, centre_bytes=len(nx_payload),
                                 safety_bytes=safety_n, nx_bytes=nx_total,
                                 nx_outer_psnr_db=nx_psnr,
                                 centre_nxdf_samples_exact=True))
            if shift == 0:
                Image.fromarray(original).save(args.private/(name+'-source.png'))
                Image.fromarray(nx).save(args.private/(name+'-nx.png'))
            for side in (272, 544, 1088):
                small = np.array(Image.fromarray(original).resize((side, side), Image.Resampling.LANCZOS))
                stereo = np.concatenate([small, small], axis=1)
                for quality in (5, 10, 20, 30):
                    for chroma in (420, 444):
                        stem = f'{name}-r{side}-q{quality}-{chroma}'
                        stream = io.BytesIO()
                        Image.fromarray(stereo).save(stream, 'JPEG', quality=quality,
                                                    subsampling=2 if chroma == 420 else 0, optimize=True)
                        encoded = stream.getvalue()
                        packet = HEADER.pack(b'NXJP', 1, 2*SIDE, SIDE, CORE, OUTER,
                                             len(nx_payload), len(encoded), 0) + nx_payload + encoded
                        restored_nx, restored_jpeg = packet_parts(packet)
                        assert restored_nx == nx_payload and restored_jpeg == encoded
                        decoded_stereo = np.array(Image.open(io.BytesIO(restored_jpeg)).convert('RGB'))
                        assert decoded_stereo.shape == (side, 2*side, 3)
                        # Resize each eye independently: no filtering across the stereo seam.
                        decoded = np.array(Image.fromarray(decoded_stereo[:, :side]).resize(
                            (SIDE, SIDE), Image.Resampling.BILINEAR))
                        hybrid = np.clip(np.rint(centre*weight + decoded*(1-weight)), 0, 255).astype(np.uint8)
                        assert np.array_equal(hybrid[core], nx[core])
                        assert np.array_equal(hybrid[periphery], decoded[periphery])
                        total = len(packet) + safety_n
                        rows.append(dict(fixture=name, jpeg_side=side, quality=quality, chroma=chroma,
                                         jpeg_bytes=len(encoded), centre_bytes=len(nx_payload),
                                         header_bytes=HEADER.size, safety_bytes=safety_n,
                                         total_bytes=total, mbps90=total*720/1e6,
                                         nx_mbps90=nx_total*720/1e6, saving_pct=100*(1-total/nx_total),
                                         jpeg_output_pixels=2*side*side,
                                         jpeg_rgb_output_bytes=2*side*side*3,
                                         outer_psnr_db=psnr(original, hybrid, periphery),
                                         nx_outer_psnr_db=nx_psnr,
                                         centre_exact=True))
                        path = args.private/(stem+'.jpg')
                        path.write_bytes(encoded)
                        if shift == 0:
                            jobs.append(dict(fixture=name, jpeg_side=side, quality=quality,
                                             chroma=chroma, files=[str(path)]))
                            if (side, quality, chroma) in ((544, 10, 420), (544, 20, 420),
                                                          (1088, 10, 420), (1088, 20, 420)):
                                Image.fromarray(hybrid).save(args.private/(stem+'.png'))
                                (args.private/(stem+'.nxjp')).write_bytes(packet)
                        # Exact length checks are important even for an offline container.
                        if side == 272 and quality == 5 and chroma == 420:
                            for malformed in (packet[:8], packet[:-1], packet+b'\0'):
                                try:
                                    packet_parts(malformed)
                                except ValueError:
                                    pass
                                else:
                                    raise AssertionError('invalid packet accepted')
            validation.append(dict(fixture=name, variants=24, centre_exact=True,
                                   serialized_parts_exact=True, malformed_lengths_rejected=3))
            print(name, '24 variants complete', flush=True)
    with (args.results/'quality.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader(); writer.writerows(rows)
    for file, value in [('fixtures.json', fixtures), ('validation.json', validation)]:
        (args.results/file).write_text(json.dumps(value, indent=2)+'\n')
    (args.private/'decode-jobs.json').write_text(json.dumps(jobs, indent=2)+'\n')
    (args.results/'method.json').write_text(json.dumps(dict(
        scope='offline source-derived JPEG periphery and retained production NXVC centre',
        eye_size=SIDE, fps_normalization=90, source_size=2160,
        source_truth_resample='shader floor coordinate convention 2160 to 2176; no lossy NV12 prefilter',
        jpeg_downsample='Lanczos', jpeg_upsample='bilinear per eye',
        jpeg_quality=[5,10,20,30], jpeg_side=[272,544,1088], jpeg_chroma=[420,444],
        jpeg_optimize=True, jpeg_library=features.version_codec('jpg'),
        round_nx_preserved_radius=CORE, round_blend_end=OUTER, blend='quintic smoothstep',
        core_equality_reference='current decoded NXVC, not original source',
        nx_centre='existing sparse NXDF tiles inside outer radius, production independent envelope selector',
        stereo='duplicated eyes, both encoded; no deduplication credit',
        jpeg_overlap='whole low-resolution frame includes redundant centre; counted in bytes',
        safety='unchanged compressed production companion, counted',
        packet_header_bytes=HEADER.size, packet_integration=False,
        excludes=['transport/FEC', 'GPU upload', 'shader sampling cost', 'network delivery', 'Pico timing'],
        variants=len(rows)), indent=2)+'\n')


if __name__ == '__main__':
    main()
