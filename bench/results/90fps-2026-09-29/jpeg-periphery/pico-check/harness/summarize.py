"""Publish numeric Pico JPEG timings without publishing the private JPEGs."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def percentile(values, q):
    values = sorted(values)
    return values[math.ceil(len(values)*q)-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--private', type=Path, required=True)
    parser.add_argument('--results', type=Path, required=True)
    args = parser.parse_args()
    args.results.mkdir(parents=True, exist_ok=True)
    summary, all_samples, hashes = [], [], {}
    for scene in ('forest', 'dark'):
        source = args.private.parent/f'{scene}-s0-r544-q20-420.jpg'
        hashes[scene] = hashlib.sha256(source.read_bytes()).hexdigest()
        for fmt in ('rgb', 'rgba'):
            suffix = '' if fmt == 'rgb' else '-rgba'
            source_csv = args.private/f'{scene}-q20{suffix}-samples.csv'
            samples = list(csv.DictReader(source_csv.open(newline='')))
            assert len(samples) == 36
            assert sum(s['phase']=='warm' for s in samples) == 12
            measured = [s for s in samples if s['phase']=='measured']
            assert len(measured) == 24 and all(s['exact']=='1' for s in samples)
            hash_key = f'{fmt}_fnv1a'
            assert len({s[hash_key] for s in samples}) == 1
            assert {int(s['image_count']) for s in samples} == {1}
            assert {int(s['encoded_bytes']) for s in samples} == {len(source.read_bytes())}
            values = [float(s['decode_us']) for s in measured]
            summary.append(dict(scene=scene, pixel_format=fmt.upper(), jpeg_width=1088,
                                jpeg_height=544, jpeg_bytes=len(source.read_bytes()),
                                source_sha256=hashes[scene], decoded_fnv1a=samples[0][hash_key],
                                decode_p50_us=percentile(values,.5),
                                decode_p95_us=percentile(values,.95),
                                decode_p99_us=percentile(values,.99)))
            for s in samples:
                all_samples.append(dict(scene=scene,pixel_format=fmt.upper(),
                                        phase=s['phase'],sample=s['sample'],
                                        decode_us=s['decode_us'],exact=s['exact']))
    for filename, values in (('pico-summary.csv', summary), ('pico-samples.csv', all_samples)):
        with (args.results/filename).open('w',newline='') as output:
            writer=csv.DictWriter(output,fieldnames=list(values[0]),lineterminator='\n')
            writer.writeheader();writer.writerows(values)
    (args.results/'checks.json').write_text(json.dumps(dict(
        jobs=4,warmup_samples=48,measured_samples=96,all_repeated_outputs_stable=True,
        all_jpeg_lengths_match_inputs=True,input_sha256=hashes),indent=2)+'\n')


if __name__ == '__main__':
    main()
