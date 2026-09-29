"""JPEG recompression of identical NXVC decoded pixels, not equal-source codec RDO.

Private source/decoded pictures remain outside the published results directory.
The compact arm is an experimental JPEG/Zstd atlas container, not standard MJPEG.
"""
import argparse
import csv
import ctypes
import hashlib
import io
import json
import math
from pathlib import Path
import struct

import numpy as np
from PIL import Image, features

PAD = 4
COLS = 16
QUALITIES = (60, 75, 85, 90, 95, 98, 100)
Z = ctypes.CDLL('libzstd.so')
Z.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
Z.ZSTD_compressBound.restype = ctypes.c_size_t
Z.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                          ctypes.c_size_t, ctypes.c_int]
Z.ZSTD_compress.restype = ctypes.c_size_t
Z.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                            ctypes.c_size_t]
Z.ZSTD_decompress.restype = ctypes.c_size_t
Z.ZSTD_isError.argtypes = [ctypes.c_size_t]
Z.ZSTD_isError.restype = ctypes.c_uint


def zstd(data):
    out = ctypes.create_string_buffer(Z.ZSTD_compressBound(len(data)))
    n = Z.ZSTD_compress(out, len(out), data, len(data), 3)
    assert not Z.ZSTD_isError(n)
    check = ctypes.create_string_buffer(len(data))
    m = Z.ZSTD_decompress(check, len(check), out, n)
    assert m == len(data) and check.raw == data
    return out.raw[:n]


def atlas_slot(side):
    # No chroma filtering across tile borders in 4:4:4; unguarded tiles align
    # to independent JPEG 8x8 transform blocks. Guarded 4:2:0 uses 16 alignment.
    alignment = 8 if PAD == 0 else 16
    return ((side + 2 * PAD + alignment - 1) // alignment) * alignment


def make_atlases(rgb, raw):
    magic, flags, count, words = struct.unpack_from('<4I', raw)
    h, w, _ = rgb.shape
    assert magic == 0x4644584e and flags == 2
    assert count == (h // 32) * (w // 32)
    assert len(raw) == 16 + 4 * (count + words)
    descriptors = np.frombuffer(raw, '<u4', count, 16)
    modes = (descriptors >> 30).astype(np.uint8)
    assert np.all(modes[(descriptors & 0x20000000) != 0] == 0)
    tiles = rgb.reshape(h // 32, 32, w // 32, 32, 3).transpose(0, 2, 1, 3, 4)
    tiles = tiles.reshape(count, 32, 32, 3)
    flat = tiles[modes == 3, 0, 0].copy()
    assert np.array_equal(tiles[modes == 3], np.broadcast_to(flat[:, None, None], tiles[modes == 3].shape))
    atlases = {}
    for mode in range(3):
        ids = np.flatnonzero(modes == mode)
        if not len(ids):
            continue
        pitch = 1 << mode
        side = 32 // pitch
        slot = atlas_slot(side)
        image = np.zeros((math.ceil(len(ids) / COLS) * slot, COLS * slot, 3), np.uint8)
        for j, tile in enumerate(ids):
            patch = tiles[tile, ::pitch, ::pitch]
            assert np.array_equal(np.repeat(np.repeat(patch, pitch, 0), pitch, 1), tiles[tile])
            guarded = np.pad(patch, ((PAD, slot-side-PAD), (PAD, slot-side-PAD), (0, 0)), mode='edge')
            y, x = divmod(j, COLS)
            image[y*slot:(y+1)*slot, x*slot:(x+1)*slot] = guarded
        atlases[mode] = image
    metadata = modes.tobytes() + flat.tobytes()
    meta_z = zstd(metadata)
    reconstructed = restore_atlases(atlases, modes, flat, h, w)
    assert np.array_equal(rgb, reconstructed), 'lossless atlas roundtrip failed'
    return atlases, modes, flat, metadata, meta_z


def restore_atlases(atlases, modes, flat, h, w):
    tiles = np.empty((len(modes), 32, 32, 3), np.uint8)
    tiles[modes == 3] = flat[:, None, None]
    for mode, image in atlases.items():
        pitch = 1 << mode
        side = 32 // pitch
        slot = atlas_slot(side)
        for j, tile in enumerate(np.flatnonzero(modes == mode)):
            y, x = divmod(j, COLS)
            patch = image[y*slot+PAD:y*slot+PAD+side, x*slot+PAD:x*slot+PAD+side]
            tiles[tile] = np.repeat(np.repeat(patch, pitch, 0), pitch, 1)
    return tiles.reshape(h//32, w//32, 32, 32, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, 3)


def decode_packet(packet):
    """Study-container self-check: reconstruct using only serialized bytes."""
    global PAD
    assert len(packet) >= 28
    magic, version, w, h, raw_n, meta_n, streams = struct.unpack_from('<4s6I', packet)
    assert magic == b'MJXA' and version in (1, 2)
    assert 0 < w <= 8192 and 0 < h <= 4096 and w % 32 == h % 32 == 0
    count = w // 32 * (h // 32)
    assert count <= raw_n <= count*4 and 0 < meta_n <= raw_n+1024 and streams <= 3
    assert len(packet) >= 28+4*streams+meta_n
    lengths = struct.unpack_from(f'<{streams}I', packet, 28)
    offset = 28+4*streams
    metadata = ctypes.create_string_buffer(raw_n)
    encoded_meta = packet[offset:offset+meta_n]
    n = Z.ZSTD_decompress(metadata, raw_n, encoded_meta, meta_n)
    assert n == raw_n and not Z.ZSTD_isError(n)
    modes = np.frombuffer(metadata.raw, np.uint8, count)
    assert np.all(modes <= 3)
    flat = np.frombuffer(metadata.raw, np.uint8, offset=count).reshape(-1,3)
    assert len(flat) == np.count_nonzero(modes == 3)
    groups = [mode for mode in range(3) if np.any(modes == mode)]
    assert len(groups) == streams
    offset += meta_n
    old_pad = PAD
    PAD = 4 if version == 1 else 0
    try:
        atlases = {}
        for mode, length in zip(groups, lengths):
            assert 0 < length <= len(packet)-offset
            atlas = np.array(Image.open(io.BytesIO(packet[offset:offset+length])).convert('RGB'))
            slot = atlas_slot(32 >> mode)
            expected = (math.ceil(np.count_nonzero(modes == mode)/COLS)*slot, COLS*slot, 3)
            assert atlas.shape == expected
            atlases[mode] = atlas
            offset += length
        assert offset == len(packet)
        return restore_atlases(atlases, modes, flat, h, w)
    finally:
        PAD = old_pad


def jpeg(image, quality, subsampling):
    stream = io.BytesIO()
    Image.fromarray(image).save(stream, format='JPEG', quality=quality,
                               subsampling=subsampling, optimize=True)
    data = stream.getvalue()
    decoded = np.array(Image.open(io.BytesIO(data)).convert('RGB'))
    return data, decoded


def metrics(reference, candidate):
    # RGB-code-value error; no claim of perceptual equivalence or photon latency.
    err = candidate.astype(np.float32) - reference.astype(np.float32)
    np.square(err, out=err)
    h, w, _ = err.shape
    cy, eye_w = h // 2, w // 2
    core_sum = native_sum = 0.
    for cx in (eye_w//2, eye_w+eye_w//2):
        core_sum += float(err[cy-64:cy+64, cx-64:cx+64].sum(dtype=np.float64))
        native_sum += float(err[cy-128:cy+128, cx-128:cx+128].sum(dtype=np.float64))
    total = float(err.sum(dtype=np.float64))
    def psnr(s, n):
        return 999.0 if s == 0 else 10*math.log10(255**2/(s/n))
    return dict(psnr_rgb_db=psnr(total, err.size),
                psnr_centre128_db=psnr(core_sum, 2*128*128*3),
                psnr_outside256_db=psnr(total-native_sum, err.size-2*256*256*3),
                rmse_rgb=math.sqrt(total/err.size))


def main():
    global PAD
    p = argparse.ArgumentParser()
    p.add_argument('--corpus', type=Path, required=True)
    p.add_argument('--decoded', type=Path, required=True)
    p.add_argument('--private', type=Path, required=True)
    p.add_argument('--results', type=Path, required=True)
    p.add_argument('--atlas-guard', type=int, choices=(0,4), default=4)
    p.add_argument('--chroma', type=int, nargs='+', choices=(420,444), default=(420,444))
    p.add_argument('--arm', nargs='+', choices=('full-raster','sample-atlas'), default=('full-raster','sample-atlas'))
    args = p.parse_args()
    PAD = args.atlas_guard
    if PAD == 0 and 420 in args.chroma:
        p.error('The unguarded aligned-block arm requires 4:4:4')
    args.private.mkdir(parents=True, exist_ok=True)
    args.results.mkdir(parents=True, exist_ok=True)
    rows, fixtures, jobs = [], [], []
    for source in sorted(args.decoded.glob('*.ppm')):
        name = source.stem
        raw = (args.corpus / (name + '.nxdf')).read_bytes()
        rgb = np.array(Image.open(source).convert('RGB'))
        h, w, _ = rgb.shape
        assert (h, w) == (2176, 4352)
        atlases, modes, flat, meta, meta_z = make_atlases(rgb, raw)
        shapes = {str(k): list(v.shape) for k, v in atlases.items()}
        fixtures.append(dict(fixture=name, nxdf_sha256=hashlib.sha256(raw).hexdigest(),
                             rgb_sha256=hashlib.sha256(rgb.tobytes()).hexdigest(),
                             duplicated_eyes=bool(np.array_equal(rgb[:, :w//2], rgb[:, w//2:])),
                             atlas_shapes=shapes, metadata_bytes=len(meta_z),
                             decoded_atlas_pixels=sum(a.shape[0]*a.shape[1] for a in atlases.values()),
                             lossless_atlas_roundtrip=True))
        Image.fromarray(rgb).save(args.private / (name + '-reference.png'))
        for chroma in args.chroma:
            sub = 0 if chroma == 444 else 2
            for q in QUALITIES:
                for arm in args.arm:
                    stem = f'{name}-{arm}-q{q}-{444 if sub == 0 else 420}'
                    payloads = []
                    if arm == 'full-raster':
                        data, decoded = jpeg(rgb, q, sub)
                        path = args.private / (stem + '.jpg')
                        path.write_bytes(data)
                        paths = [str(path)]
                        byte_count = len(data)
                        pixels = h*w
                    else:
                        decoded_atlases, paths = {}, []
                        for mode, atlas in atlases.items():
                            data, decoded_atlases[mode] = jpeg(atlas, q, sub)
                            payloads.append(data)
                            path = args.private / (stem + f'-mode{mode}.jpg')
                            path.write_bytes(data)
                            paths.append(str(path))
                        decoded = restore_atlases(decoded_atlases, modes, flat, h, w)
                        header = struct.pack('<4s6I', b'MJXA', 1 if PAD == 4 else 2, w, h, len(meta), len(meta_z), len(payloads))
                        header += b''.join(struct.pack('<I', len(data)) for data in payloads)
                        packet = header + meta_z + b''.join(payloads)
                        (args.private / (stem + '.mjxa')).write_bytes(packet)
                        byte_count = len(packet)
                        pixels = sum(a.shape[0]*a.shape[1] for a in atlases.values())
                    row = dict(fixture=name, arm=arm, quality=q, chroma=444 if sub == 0 else 420,
                               atlas_guard=PAD if arm == 'sample-atlas' else -1,
                               detail_bytes=byte_count, detail_mbps90=byte_count*8*90/1e6,
                               jpeg_decode_pixels=pixels, metadata_bytes=len(meta_z) if arm == 'sample-atlas' else 0,
                               **metrics(rgb, decoded))
                    rows.append(row)
                    if q in (85, 95, 100) and name.endswith('s0'):
                        # Private, native-size crops; no artificial sharpening or blur.
                        Image.fromarray(decoded[960:1216, 960:1216]).save(args.private / (stem + '-centre.png'))
                    jobs.append(dict(fixture=name, arm=arm, quality=q, chroma=row['chroma'], files=paths))
        print(name, 'done', shapes, flush=True)
    with (args.results/'quality.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator='\n'); writer.writeheader(); writer.writerows(rows)
    (args.results/'fixtures.json').write_text(json.dumps(fixtures, indent=2)+'\n')
    (args.private/'decode-jobs.json').write_text(json.dumps(jobs, indent=2)+'\n')
    (args.results/'jpeg-method.json').write_text(json.dumps(dict(
        scope='Recompression of the same reconstructed NXDF RGB, additional JPEG error only',
        image_size=[4352,2176], fps_for_normalization=90, quality=list(QUALITIES),
        jpeg_library=features.version_codec('jpg'), pillow_turbo=features.check_feature('libjpeg_turbo'),
        jpeg_optimize=True, chroma=args.chroma, atlas_guard=PAD, atlas_columns=COLS,
        atlas_metadata='Zstd level 3 tile pitch map and exact inline flat colours',
        atlas_scope='JPEG/Zstd hybrid prototype, not standard MJPEG or integrated WiVRn',
        safety='Common safety payload added separately in summary',
        gpu_or_pico_timing=False, baseline_already_lossy=True), indent=2)+'\n')


if __name__ == '__main__':
    main()
