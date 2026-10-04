#!/usr/bin/env python3
"""Payload-only scaling and loss stress for two existing ASTC files."""
import ctypes
import ctypes.util
import argparse
import csv
import hashlib
import math
import random
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
TILES_PX = (64, 128, 256)
MTU = 1400
ASTC_PACKET_HEADER = 24
REGION_HEADER = 8  # frame_id:u32, region_id:u16, compressed_bytes:u16
K, DEPTH = 8, 4  # existing moderate single-XOR/interleaved layout
TRIALS = 300
REPEATED_FRAMES = 36

lib = ctypes.CDLL(ctypes.util.find_library('zstd'))
lib.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
lib.ZSTD_compressBound.restype = ctypes.c_size_t
lib.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
lib.ZSTD_compress.restype = ctypes.c_size_t
lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
lib.ZSTD_decompress.restype = ctypes.c_size_t
lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
lib.ZSTD_isError.restype = ctypes.c_uint


def zstd(data, level=3):
    src = ctypes.create_string_buffer(data)
    cap = lib.ZSTD_compressBound(len(data))
    dst = ctypes.create_string_buffer(cap)
    n = lib.ZSTD_compress(dst, cap, src, len(data), level)
    if lib.ZSTD_isError(n):
        raise RuntimeError('Zstd compression failed')
    return dst.raw[:n]


def unzstd(data, expected):
    src = ctypes.create_string_buffer(data)
    dst = ctypes.create_string_buffer(expected)
    n = lib.ZSTD_decompress(dst, expected, src, len(data))
    if lib.ZSTD_isError(n) or n != expected:
        raise RuntimeError('Zstd region round-trip failed')
    return dst.raw[:n]


def parse_astc(path):
    data = path.read_bytes()
    if data[:4] != bytes((0x13, 0xAB, 0xA1, 0x5C)) or data[4:7] != bytes((8, 8, 1)):
        raise ValueError(f'{path.name}: expected ASTC 8x8 2D file')
    dim = lambda off: int.from_bytes(data[off:off + 3], 'little')
    width, height, depth = dim(7), dim(10), dim(13)
    bw, bh = (width + 7) // 8, (height + 7) // 8
    blocks = data[16:]
    if depth != 1 or len(blocks) != bw * bh * 16:
        raise ValueError(f'{path.name}: malformed ASTC dimensions/payload')
    return width, height, bw, bh, blocks


def region_parts(blocks, bw, bh, tile_px):
    tile_blocks = tile_px // 8
    regions = []
    for by in range(0, bh, tile_blocks):
        for bx in range(0, bw, tile_blocks):
            tw, th = min(tile_blocks, bw - bx), min(tile_blocks, bh - by)
            part = bytearray()
            for y in range(by, by + th):
                off = (y * bw + bx) * 16
                part.extend(blocks[off:off + tw * 16])
            area = min(tile_px, bw * 8 - bx * 8) * min(tile_px, bh * 8 - by * 8)
            regions.append((bx, by, tw, th, area, bytes(part)))
    return regions


def fec_layout(data_count):
    groups = []
    data_start = wire_start = 0
    while data_start < data_count:
        count = min(K * DEPTH, data_count - data_start)
        parity_count = min(DEPTH, count)
        for group_id in range(parity_count):
            members = [(wire_start + i, data_start + i)
                       for i in range(count) if i % DEPTH == group_id]
            groups.append((members, wire_start + count + group_id))
        data_start += count
        wire_start += count + parity_count
    return groups, wire_start


def packet_plan(path):
    width, height, bw, bh, astc = parse_astc(path)
    whole = zstd(astc)
    whole_len = ASTC_PACKET_HEADER + len(whole)
    whole_count = math.ceil(whole_len / MTU)
    variants = {'whole': {
        'width': width, 'height': height, 'blocks': astc, 'regions': None,
        'data_bytes': len(whole), 'header_bytes': ASTC_PACKET_HEADER,
        'data_count': whole_count, 'area_by_region': [width * height],
    }}
    # Store just fragment-to-region identity; never save private compressed bytes.
    variants['whole']['owners'] = [None] * whole_count
    for tile_px in TILES_PX:
        tile_data, owners, area_by_region, raw_by_region = 0, [], [], []
        regions = region_parts(astc, bw, bh, tile_px)
        for rid, (bx, by, tw, th, area, raw) in enumerate(regions):
            compressed = zstd(raw)
            record_len = REGION_HEADER + len(compressed)
            count = math.ceil(record_len / MTU)
            tile_data += len(compressed)
            owners.extend([rid] * count)
            area_by_region.append(area)
            raw_by_region.append((bx, by, tw, th, area, raw, compressed))
        variants[f'region_{tile_px}'] = {
            'width': width, 'height': height, 'blocks': astc,
            'regions': raw_by_region, 'data_bytes': tile_data,
            'header_bytes': len(regions) * REGION_HEADER,
            'data_count': len(owners), 'area_by_region': area_by_region,
            'owners': owners,
        }
        # Prove each compressed tile independently decodes and exact block merge
        # reconstructs all original ASTC bytes, including right/bottom partial tiles.
        merged = bytearray(len(astc))
        for bx, by, tw, th, area, raw, packed in raw_by_region:
            decoded = unzstd(packed, len(raw))
            assert decoded == raw
            row_bytes = tw * 16
            for row in range(th):
                src = row * row_bytes
                dst = ((by + row) * bw + bx) * 16
                merged[dst:dst + row_bytes] = decoded[src:src + row_bytes]
        assert merged == astc, f'{tile_px}px ASTC region merge did not reconstruct {path.name}'
    return variants


def loss_pattern(n, rng, target, burst=0):
    if not burst:
        return [rng.random() < target for _ in range(n)]
    start_p = target / (burst - target * (burst - 1)) if target else 0
    result, remaining = [], 0
    for _ in range(n):
        if remaining:
            result.append(True)
            remaining -= 1
        elif rng.random() < start_p:
            result.append(True)
            remaining = burst - 1
        else:
            result.append(False)
    return result


def delivery(plan, rng, target, burst):
    owners = plan['owners']
    groups, wire_count = fec_layout(len(owners))
    lost = loss_pattern(wire_count, rng, target, burst)
    recovered = [True] * len(owners)
    for members, parity in groups:
        dropped = [di for wi, di in members if lost[wi]]
        if len(dropped) == 1 and not lost[parity]:
            dropped.clear()
        for di in dropped:
            recovered[di] = False
    if plan['regions'] is None:
        complete = float(all(recovered))
        return complete, complete, sum(lost), wire_count
    good = [True] * len(plan['regions'])
    for ok, rid in zip(recovered, owners):
        if not ok:
            good[rid] = False
    fresh = sum(area for ok, area in zip(good, plan['area_by_region']) if ok)
    return fresh / (plan['width'] * plan['height']), float(all(good)), sum(lost), wire_count


def scenarios():
    return [('iid_2pct', 0.02, 0), ('iid_5pct', 0.05, 0),
            ('burst8_2pct', 0.02, 8), ('burst16_2pct', 0.02, 16)]


def run(inputs):
    payload_rows, loss_rows = [], []
    for sample, path in inputs.items():
        plans = packet_plan(path)
        base_bytes = None
        for name, plan in plans.items():
            groups, total_packets = fec_layout(plan['data_count'])
            parity_bytes = len(groups) * MTU
            data_payload_bytes = plan['data_bytes'] + plan['header_bytes']
            estimate = data_payload_bytes + parity_bytes
            if name == 'whole':
                base_bytes = estimate
            payload_rows.append({
                'sample': sample, 'width': plan['width'], 'height': plan['height'],
                'mode': name, 'regions': len(plan['regions']) if plan['regions'] is not None else 1,
                'data_payload_bytes': data_payload_bytes,
                'zstd3_block_data_bytes': plan['data_bytes'],
                'explicit_header_bytes': plan['header_bytes'],
                'data_fragments_1400': plan['data_count'],
                'fec_groups': len(groups), 'parity_payload_bytes_est': parity_bytes,
                'total_payload_bytes_est': estimate,
            })
            for label, rate, burst in scenarios():
                seed = int.from_bytes(hashlib.sha256(f'{sample}:{name}:{label}'.encode()).digest()[:4], 'little')
                rng = random.Random(20261004 + seed)
                fresh_total = complete_total = lost_total = packet_total = 0
                for _ in range(TRIALS):
                    for _ in range(REPEATED_FRAMES):
                        fresh, complete, lost, packets = delivery(plan, rng, rate, burst)
                        fresh_total += fresh
                        complete_total += complete
                        lost_total += lost
                        packet_total += packets
                frames = TRIALS * REPEATED_FRAMES
                loss_rows.append({
                    'sample': sample, 'mode': name, 'loss_model': label,
                    'requested_loss': rate, 'burst_packets': burst,
                    'trials': TRIALS, 'repeated_static_frames_per_trial': REPEATED_FRAMES,
                    'frames_simulated': frames,
                    'mean_fresh_area_fraction': fresh_total / frames,
                    'fully_fresh_frame_fraction': complete_total / frames,
                    'observed_packet_loss_fraction': lost_total / packet_total,
                    'mean_wire_packets_per_frame': packet_total / frames,
                })
        assert base_bytes
        for row in payload_rows:
            if row['sample'] == sample:
                row['penalty_vs_whole_percent'] = (row['total_payload_bytes_est'] / base_bytes - 1) * 100

    with (HERE / 'native_payload_bytes.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=payload_rows[0].keys())
        writer.writeheader()
        writer.writerows(payload_rows)
    with (HERE / 'native_loss_scaling.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=loss_rows[0].keys())
        writer.writeheader()
        writer.writerows(loss_rows)
    print('payload rows', len(payload_rows), 'loss rows', len(loss_rows),
          'trials', TRIALS, 'repeated frames', REPEATED_FRAMES)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dark-q6', type=Path, required=True, help='private/native ASTC input; pixels are never written')
    parser.add_argument('--forest-q6', type=Path, required=True, help='private/native ASTC input; pixels are never written')
    args = parser.parse_args()
    run({'dark_q6': args.dark_q6, 'forest_q6': args.forest_q6})
