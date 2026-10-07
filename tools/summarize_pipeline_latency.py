#!/usr/bin/env python3
"""Measure encode-to-feedback latency using explicit NX frame identities.

Inputs must contain one complete session from its first sent frame. Feedback
clocks are converted to server time by WiVRn; blit is selection, not photons.
"""
import argparse
import csv
import gzip
import json


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    low = int(position)
    return values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (position - low)


def summarize(records, codec, warmup=10):
    enc, frames, mapping = {}, {}, {}
    for row in records:
        if len(row) < 4 or row[3] != '0':
            continue
        event, frame, timestamp = row[0], int(row[1]), int(row[2])
        if event == 'nx_frame_map':
            if len(row) != 5 or not 0 <= int(row[4]) <= 65535:
                raise ValueError('invalid NX wire mapping')
            wire = int(row[4])
            if frame in mapping and mapping[frame][1] != wire:
                raise ValueError('conflicting outer frame mapping')
            mapping[frame] = min(mapping.get(frame, (timestamp, wire)), (timestamp, wire))
        elif event == 'encode_begin':
            enc[frame] = min(timestamp, enc.get(frame, timestamp))
        elif event in ('receive_begin', 'decode_end', 'blit'):
            events = frames.setdefault(frame, {})
            events[event] = min(timestamp, events.get(event, timestamp))
    arrivals = [events['receive_begin'] for events in frames.values() if 'receive_begin' in events]
    if not arrivals:
        raise ValueError('no stream-0 arrivals')
    cutoff = min(arrivals) + warmup * 1e9
    wire_to_outer, epoch, previous = {}, 0, None
    if codec == 'nx':
        if not mapping:
            raise ValueError('NX input has no nx_frame_map rows')
        for outer, (timestamp, wire) in sorted(mapping.items(), key=lambda item: item[1][0]):
            if previous is not None:
                delta = (wire - previous) & 65535
                if delta == 0 or delta >= 32768:
                    raise ValueError('ambiguous or non-forward wire mapping')
                if wire < previous:
                    epoch += 65536
            extended = epoch + wire
            if extended in wire_to_outer:
                raise ValueError('conflicting wire frame mapping')
            wire_to_outer[extended] = outer
            previous = wire
    stages = {stage: [] for stage in ('receive_begin', 'decode_end', 'blit')}
    unmatched_arrivals = 0
    for wire, events in frames.items():
        if events.get('receive_begin', -1) < cutoff:
            continue
        if codec == 'nx' and wire not in wire_to_outer:
            raise ValueError(f'unmapped NX feedback frame {wire}')
        outer = wire_to_outer[wire] if codec == 'nx' else wire
        if outer not in enc:
            if 'decode_end' in events or 'blit' in events:
                raise ValueError(f'missing encode_begin for decoded/selected frame {outer}')
            unmatched_arrivals += 1
            continue
        for stage in stages:
            if stage in events:
                duration = (events[stage] - enc[outer]) / 1e6
                if duration < 0:
                    raise ValueError(f'negative {stage} latency for frame {outer}')
                stages[stage].append(duration)
    return {'codec': codec, 'warmup_s': warmup,
            'unmatched_arrivals_without_decode_or_selection': unmatched_arrivals, 'stages': {
        stage: {'count': len(values), 'p50_p95_p99_ms':
                [round(percentile(values, q), 3) for q in (.5, .95, .99)] if values else []}
        for stage, values in stages.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv')
    parser.add_argument('--codec', choices=('nx', 'hevc'), required=True)
    parser.add_argument('--warmup', type=float, default=10)
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error('warmup must be nonnegative')
    try:
        opener = gzip.open if args.csv.endswith('.gz') else open
        with opener(args.csv, 'rt', newline='') as source:
            result = summarize(csv.reader(source), args.codec, args.warmup)
    except ValueError as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps({'file': args.csv, **result}, sort_keys=True))


if __name__ == '__main__':
    main()
