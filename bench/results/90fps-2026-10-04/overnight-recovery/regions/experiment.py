#!/usr/bin/env python3
"""Bounded packet-loss/byte comparison; not a decoder or runtime model."""
import ctypes
import ctypes.util
import argparse
import csv
import math
import random
import struct
import subprocess
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageStat

OUT = Path(__file__).resolve().parent
N = 36
W = H = 512
BLOCK = 8
BLOCK_BYTES = 16
MTU_PAYLOAD = 1400
REGION_HEADER = 8  # frame id u32, region id u16, compressed bytes u16
WHOLE_HEADER = 24  # existing NXASTC packet header
TRIALS = 2000

z = ctypes.CDLL(ctypes.util.find_library('zstd'))
z.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
z.ZSTD_compressBound.restype = ctypes.c_size_t
z.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                            ctypes.c_size_t, ctypes.c_int]
z.ZSTD_compress.restype = ctypes.c_size_t
z.ZSTD_isError.argtypes = [ctypes.c_size_t]
z.ZSTD_isError.restype = ctypes.c_uint


def zstd3(data):
    src = ctypes.create_string_buffer(data)
    cap = z.ZSTD_compressBound(len(data))
    dst = ctypes.create_string_buffer(cap)
    size = z.ZSTD_compress(dst, cap, src, len(data), 3)
    if z.ZSTD_isError(size):
        raise RuntimeError('ZSTD_compress failed')
    return dst.raw[:size]


def read_frames(src_dir, astcenc):
    blocks, rgba = [], []
    decoded_dir = OUT / 'astcenc_decode'
    decoded_dir.mkdir(exist_ok=True)
    for i in range(N):
        astc = (src_dir / f'{i:02}.astc').read_bytes()
        # ASTC 8x8 standard header; input is 512x512, 4096 blocks, 16 bytes each.
        assert astc[:4] == bytes((0x13, 0xAB, 0xA1, 0x5C))
        assert astc[4:7] == bytes((8, 8, 1))
        assert len(astc) == 16 + 4096 * BLOCK_BYTES
        blocks.append(astc[16:])
        decoded_path = decoded_dir / f'{i:02}.png'
        subprocess.run([astcenc, '-dl', str(src_dir / f'{i:02}.astc'), str(decoded_path)],
                       check=True, stdout=subprocess.DEVNULL)
        with Image.open(decoded_path) as decoded:
            assert decoded.size == (W, H)
            rgba.append(decoded.convert('RGBA').tobytes())
    return blocks, rgba


def regions(frame, px):
    blocks_per = px // BLOCK
    across = W // px
    out = []
    for ry in range(across):
        for rx in range(across):
            chunk = bytearray()
            for y in range(ry * blocks_per, (ry + 1) * blocks_per):
                start = (y * (W // BLOCK) + rx * blocks_per) * BLOCK_BYTES
                chunk.extend(frame[start:start + blocks_per * BLOCK_BYTES])
            out.append(bytes(chunk))
    return out


def make_records(frames, px):
    records = []
    for frame_id, frame in enumerate(frames):
        parts = regions(frame, px)
        encoded = []
        for region_id, part in enumerate(parts):
            compressed = zstd3(part)
            header = struct.pack('<IHH', frame_id, region_id, len(compressed))
            encoded.append((header + compressed, region_id))
        records.append(encoded)
    return records


def packets_for_records(records):
    packets = []
    for record, region_id in records:
        fragments = [record[i:i + MTU_PAYLOAD] for i in range(0, len(record), MTU_PAYLOAD)]
        packets.extend((region_id, j, len(fragments)) for j in range(len(fragments)))
    return packets


def loss_mask(total, rng, rate, burst=0):
    lost = [False] * total
    if burst:
        # Gilbert-like fixed burst length; start probability is calibrated for
        # the requested stationary packet loss fraction.
        q = rate / (burst - rate * (burst - 1)) if rate else 0
        remaining = 0
        for i in range(total):
            if remaining:
                lost[i] = True
                remaining -= 1
            elif rng.random() < q:
                lost[i] = True
                remaining = burst - 1
    else:
        lost = [rng.random() < rate for _ in range(total)]
    return lost


def layout_packets(data_packets):
    # Existing moderate layout: 8 data per XOR group, four-way interleaving.
    # Data shards in each 32-shard block go first; its four parity shards follow.
    groups = []
    data_start = wire_start = 0
    for start in range(0, len(data_packets), 8 * 4):
        count = min(8 * 4, len(data_packets) - start)
        parity_count = min(4, count)
        for group_id in range(parity_count):
            members = [(wire_start + i, data_start + i) for i in range(count) if i % 4 == group_id]
            groups.append((members, wire_start + count + group_id))
        data_start += count
        wire_start += count + parity_count
    return groups, wire_start


def apply_fec(data_packets, groups, lost):
    delivered = [True] * len(data_packets)
    for indices, parity_index in groups:
        lost_data = [data_index for wire_index, data_index in indices if lost[wire_index]]
        parity_lost = lost[parity_index]
        if len(lost_data) == 1 and not parity_lost:
            lost_data.clear()
        for data_index in lost_data:
            delivered[data_index] = False
    return delivered


def check_fec_model():
    groups, wire_count = layout_packets([None] * 32)
    assert wire_count == 36
    one = [False] * wire_count
    one[5] = True
    assert all(apply_fec([None] * 32, groups, one))
    four = [False] * wire_count
    for i in range(4):
        four[i] = True
    assert all(apply_fec([None] * 32, groups, four))  # four-way interleave
    two_same_group = [False] * wire_count
    two_same_group[0] = two_same_group[4] = True
    got = apply_fec([None] * 32, groups, two_same_group)
    assert not got[0] and not got[4] and sum(not x for x in got) == 2


def fresh_regions(records, rng, rate, burst=0):
    packets = packets_for_records(records)
    groups, total = layout_packets(packets)
    lost = loss_mask(total, rng, rate, burst)
    delivered = apply_fec(packets, groups, lost)
    complete = [True] * (max((rid for _, rid in records), default=-1) + 1)
    idx = 0
    for rid, _, _ in packets:
        if rid is not None and not delivered[idx]:
            complete[rid] = False
        idx += 1
    return complete, sum(lost), total


def whole_result(payload, rng, rate, burst=0):
    wire = payload
    chunks = [wire[i:i + MTU_PAYLOAD] for i in range(0, len(wire), MTU_PAYLOAD)]
    groups, total = layout_packets(chunks)
    lost = loss_mask(total, rng, rate, burst)
    return all(apply_fec(chunks, groups, lost)), sum(lost), total


def payload_stats(whole_payloads, region_records):
    rows = []
    for i, full in enumerate(whole_payloads):
        full_wire = WHOLE_HEADER + len(full)
        whole_data = math.ceil(full_wire / MTU_PAYLOAD)
        whole_groups = len(layout_packets([None] * whole_data)[0])
        rows.append((i, 'whole', 0, len(full), WHOLE_HEADER, whole_data,
                     whole_groups, whole_groups * MTU_PAYLOAD,
                     full_wire + whole_groups * MTU_PAYLOAD))
        for px in (64, 128):
            records = region_records[px][i]
            data_bytes = sum(map(lambda item: len(item[0]), records))
            packet_count = sum(math.ceil(len(record) / MTU_PAYLOAD) for record, _ in records)
            group_count = len(layout_packets([None] * packet_count)[0])
            rows.append((i, 'region', px, data_bytes - len(records) * REGION_HEADER,
                         len(records) * REGION_HEADER, packet_count, group_count,
                         group_count * MTU_PAYLOAD,
                         data_bytes + group_count * MTU_PAYLOAD))
    return rows


def simulate(whole_payloads, region_records):
    rows = []
    for burst in (0, 4):
        for rate in (0.002, 0.01, 0.02):
            accum = {'whole_fresh_frames': 0, 'r64_fresh_pixels': 0, 'r128_fresh_pixels': 0,
                     'whole_tx_packets': 0, 'r64_tx_packets': 0, 'r128_tx_packets': 0, 'lost_packets': 0}
            total_frames = 0
            for trial in range(TRIALS):
                rng = random.Random((20261004 << 24) + burst * 1_000_000 + int(rate * 1_000_000) * 10_000 + trial)
                for frame_index in range(N):
                    whole_payload = whole_payloads[frame_index]
                    whole_frame = whole_payload[:0] + bytes(WHOLE_HEADER) + whole_payload
                    whole_fresh, lost, packets = whole_result(whole_frame, rng, rate, burst)
                    accum['whole_fresh_frames'] += int(whole_fresh)
                    accum['whole_tx_packets'] += packets
                    accum['lost_packets'] += lost
                    for px, name in ((64, 'r64'), (128, 'r128')):
                        fresh, lost, packets = fresh_regions(region_records[px][frame_index], rng, rate, burst)
                        accum[name + '_fresh_pixels'] += sum(fresh) / len(fresh)
                        accum[name + '_tx_packets'] += packets
                        accum['lost_packets'] += lost
                    total_frames += 1
            rows.append({
                'loss_model': 'burst4' if burst else 'iid',
                'target_packet_loss': rate,
                'trials': TRIALS,
                'frames': total_frames,
                'whole_fresh_frame_fraction': accum['whole_fresh_frames'] / total_frames,
                'region64_fresh_pixel_fraction': accum['r64_fresh_pixels'] / total_frames,
                'region128_fresh_pixel_fraction': accum['r128_fresh_pixels'] / total_frames,
                'whole_mean_packets_per_frame': accum['whole_tx_packets'] / total_frames,
                'region64_mean_packets_per_frame': accum['r64_tx_packets'] / total_frames,
                'region128_mean_packets_per_frame': accum['r128_tx_packets'] / total_frames,
                'mean_observed_loss_fraction_all_modes': accum['lost_packets'] /
                    (total_frames * (accum['whole_tx_packets'] + accum['r64_tx_packets'] + accum['r128_tx_packets']) /
                     total_frames),
            })
    return rows


def merge_astc_region(retained, incoming, region_id):
    blocks_per_region = 64 // BLOCK
    blocks_per_row = W // BLOCK
    rx, ry = region_id % (W // 64), region_id // (W // 64)
    for by in range(blocks_per_region):
        start = ((ry * blocks_per_region + by) * blocks_per_row + rx * blocks_per_region) * BLOCK_BYTES
        size = blocks_per_region * BLOCK_BYTES
        retained[start:start + size] = incoming[start:start + size]


def illustration(blocks, rgba, records64, astc_header):
    # Make a visible 4-packet burst trace. This uses source RGBA solely as an
    # illustration of patch composition using ASTC-CLI-decoded frames.
    count = 0
    packet_lists = [packets_for_records(records) for records in records64]
    total = sum(layout_packets(p)[1] for p in packet_lists[1:])
    lost = loss_mask(total, random.Random(20261004), 0.01, 4)
    cursor = 0
    previous_whole = Image.frombytes('RGBA', (W, H), rgba[0])
    previous_region = previous_whole.copy()
    retained_astc = bytearray(blocks[0])
    frames = []
    best_score, best_frame = -1, None
    best_astc = None
    for fi in range(1, N):
        current = Image.frombytes('RGBA', (W, H), rgba[fi])
        packets = packet_lists[fi]
        groups, n = layout_packets(packets)
        local_lost = lost[cursor:cursor + n]
        delivered = apply_fec(packets, groups, local_lost)
        cursor += n
        if all(delivered):
            previous_whole = current.copy()
        for ok, (rid, _, _) in zip(delivered, packets):
            if not ok or rid is None:
                continue
            merge_astc_region(retained_astc, blocks[fi], rid)
            across = W // 64
            x, y = (rid % across) * 64, (rid // across) * 64
            previous_region.paste(current.crop((x, y, x + 64, y + 64)), (x, y))
        canvas = Image.new('RGB', (W * 2, H + 38), 'black')
        canvas.paste(previous_whole.convert('RGB'), (0, 38))
        canvas.paste(previous_region.convert('RGB'), (W, 38))
        draw = ImageDraw.Draw(canvas)
        draw.text((10, 12), 'Whole frame, hold last complete', fill='white')
        draw.text((W + 10, 12), '64px region patches, illustration', fill='white')
        draw.text((W * 2 - 136, 12), f'frame {fi:02}', fill='white')
        frames.append(canvas)
        difference = ImageChops.difference(previous_whole.convert('RGB'), previous_region.convert('RGB'))
        score = sum(ImageStat.Stat(difference).sum)
        if score > best_score:
            best_score, best_frame, best_astc = score, canvas.copy(), bytes(retained_astc)
    frames[0].save(OUT / 'hold_vs_regions.gif', save_all=True, append_images=frames[1:], duration=80, loop=0)
    best_frame.save(OUT / 'hold_vs_regions.png')
    (OUT / 'region_assembly_demo.astc').write_bytes(astc_header + best_astc)


def write_csv(path, rows):
    with path.open('w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['frame', 'mode', 'region_px', 'zstd3_data_bytes', 'explicit_header_bytes',
                         'data_fragments', 'fec_groups', 'fec_parity_bytes_at_1400', 'wire_payload_bytes_est'])
        writer.writerows(rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--sequence-dir', type=Path, required=True,
                        help='directory with procedural 00.astc through 35.astc frames')
    parser.add_argument('--astcenc', default='astcenc-native', help='ASTC decoder executable')
    args = parser.parse_args()
    check_fec_model()
    blocks, rgba = read_frames(args.sequence_dir, args.astcenc)
    probe = bytearray(len(blocks[0]))
    for region_id, _ in enumerate(regions(blocks[0], 64)):
        merge_astc_region(probe, blocks[0], region_id)
    assert probe == blocks[0], '64px ASTC region merge did not reconstruct the source blocks'
    whole_payloads = [zstd3(frame) for frame in blocks]
    records = {px: [
        [(struct.pack('<IHH', frame_id, rid, len(c := zstd3(region))) + c, rid)
         for rid, region in enumerate(regions(frame, px))]
        for frame_id, frame in enumerate(blocks)] for px in (64, 128)}
    stats = payload_stats(whole_payloads, records)
    write_csv(OUT / 'payload_bytes.csv', stats)
    loss_rows = simulate(whole_payloads, records)
    with (OUT / 'loss_replay.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=loss_rows[0].keys())
        writer.writeheader()
        writer.writerows(loss_rows)
    illustration(blocks, rgba, records[64], (args.sequence_dir / '00.astc').read_bytes()[:16])
    print('frames', len(blocks), 'region sizes', '64/128', 'trials/model/rate', TRIALS)
    print('artifacts', *(str(OUT / p) for p in ('payload_bytes.csv', 'loss_replay.csv', 'hold_vs_regions.gif', 'hold_vs_regions.png')))
