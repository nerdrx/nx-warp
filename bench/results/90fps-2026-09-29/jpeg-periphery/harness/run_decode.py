#!/usr/bin/env python3
"""Run the fixed-seed, serial libjpeg-turbo CPU decode comparison."""
import csv, datetime as dt, hashlib, json, pathlib, platform, random, subprocess

REPO = pathlib.Path(__file__).resolve().parents[5]
MANIFEST = REPO / 'scratch/jpeg-periphery/decode-jobs.json'
SOURCE = REPO / 'scratch/mjpeg-decode-agent/jpeg_decode_bench.cpp'
BINARY = REPO / 'scratch/mjpeg-decode-agent/jpeg_decode_bench'
SCRATCH = REPO / 'scratch/jpeg-periphery/timing'
PUBLIC = pathlib.Path(__file__).resolve().parents[1]
SEED = 20260929


def utcnow():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec='milliseconds')


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def cpu_model():
    for line in pathlib.Path('/proc/cpuinfo').read_text().splitlines():
        if line.lower().startswith('model name'):
            return line.split(':', 1)[1].strip()
    return platform.processor() or 'unknown'


def top_processes():
    result = subprocess.run(['ps', '-eo', 'pcpu,comm', '--sort=-pcpu'],
                            text=True, capture_output=True, check=True)
    rows = []
    for line in result.stdout.splitlines()[1:6]:
        cols = line.split(None, 1)
        if len(cols) == 2:
            rows.append({'cpu_percent': float(cols[0]), 'name': cols[1]})
    return rows


def percentile(values, q):
    v = sorted(values)
    return v[max(1, int(q * len(v) + 0.999999)) - 1]


def main():
    jobs = json.loads(MANIFEST.read_text())
    if len(jobs) != 48:
        raise RuntimeError(f'expected 48 jobs, found {len(jobs)}')
    jobs = list(jobs)
    random.Random(SEED).shuffle(jobs)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    start = utcnow()
    order = {'seed': SEED, 'started_utc': start, 'jobs': jobs}
    (SCRATCH / 'run-order.json').write_text(json.dumps(order, indent=2) + '\n')
    meta = {
        'status': 'running', 'started_utc': start, 'ended_utc': None,
        'job_count': len(jobs), 'jobs_completed': 0, 'shuffle_seed': SEED,
        'protocol': 'one serial process per job; 12 warmups + 24 measured whole-frame samples; timer covers ordered tj3Decompress8 calls only; RGB hashing and checks outside timer',
        'scope': 'CPU libjpeg-turbo JPEG decode to RGB888 only; excludes GPU, Android hardware decode, NXVC, app pipeline, transport, presentation, and live FPS',
        'library': 'libjpeg-turbo 3.2.0', 'cpu_model': cpu_model(),
        'platform': platform.platform(), 'machine': platform.machine(),
        'host_load_start_top5': top_processes(),
        'manifest': str(MANIFEST), 'manifest_sha256': sha256(MANIFEST),
        'source': str(SOURCE), 'source_sha256': sha256(SOURCE),
        'binary': str(BINARY), 'binary_sha256': sha256(BINARY),
        'runner': str(pathlib.Path(__file__).resolve()), 'runner_sha256': sha256(pathlib.Path(__file__).resolve()),
        'scratch_run_dir': str(SCRATCH),
    }
    method_path = PUBLIC / 'jpeg-timing-method.json'
    method_path.write_text(json.dumps(meta, indent=2) + '\n')
    summaries, all_samples = [], []
    try:
        for idx, job in enumerate(jobs, 1):
            files = [REPO / f for f in job['files']]
            if any(not p.is_file() for p in files):
                raise FileNotFoundError(next(str(p) for p in files if not p.is_file()))
            tag = f"{job['fixture']}__r{job['jpeg_side']}__q{job['quality']}__c{job['chroma']}"
            raw_path, log_path = SCRATCH / f'{tag}.csv', SCRATCH / f'{tag}.log'
            job_start = utcnow()
            with log_path.open('w') as log:
                proc = subprocess.run([str(BINARY), *(str(p) for p in files), str(raw_path)],
                                      cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
            if proc.returncode:
                raise RuntimeError(f'job failed ({proc.returncode}): {tag}; see {log_path}')
            rows = list(csv.DictReader(raw_path.open(newline='')))
            measured = [r for r in rows if r['phase'] == 'measured']
            if len(measured) != 24 or any(r['exact'] != '1' for r in rows):
                raise RuntimeError(f'invalid samples: {tag} ({len(measured)} measured)')
            with log_path.open() as log:
                text = log.read()
            image_rows = [line for line in text.splitlines() if line.startswith('image=')]
            pixels = 0
            jpeg_bytes = int(measured[0]['encoded_bytes'])
            for line in image_rows:
                fields = dict(field.split('=', 1) for field in line.split(',') if '=' in field)
                pixels += int(fields['width']) * int(fields['height'])
            values = [float(r['decode_us']) for r in measured]
            summaries.append({
                'fixture': job['fixture'], 'jpeg_side': job['jpeg_side'],
                'quality': job['quality'], 'chroma': job['chroma'],
                'jpeg_bytes': jpeg_bytes, 'jpeg_pixels': pixels,
                'decode_p50_us': percentile(values, .50),
                'decode_p95_us': percentile(values, .95),
                'decode_p99_us': percentile(values, .99),
            })
            for r in rows:
                all_samples.append({
                    'fixture': job['fixture'], 'jpeg_side': job['jpeg_side'],
                    'quality': job['quality'], 'chroma': job['chroma'], **r,
                })
            meta.update({'jobs_completed': idx, 'last_job': tag,
                         'last_job_started_utc': job_start, 'last_job_ended_utc': utcnow()})
            method_path.write_text(json.dumps(meta, indent=2) + '\n')
            print(f"[{idx}/48] {tag}: p50={summaries[-1]['decode_p50_us']:.3f} us "
                  f"p95={summaries[-1]['decode_p95_us']:.3f} us "
                  f"p99={summaries[-1]['decode_p99_us']:.3f} us", flush=True)

        summaries.sort(key=lambda r: (r['fixture'], r['jpeg_side'], r['quality'], r['chroma']))
        summary_path = PUBLIC / 'jpeg-decode-summary.csv'
        with summary_path.open('w', newline='') as f:
            fields = ['fixture','jpeg_side','quality','chroma','jpeg_bytes','jpeg_pixels',
                      'decode_p50_us','decode_p95_us','decode_p99_us']
            w = csv.DictWriter(f, fieldnames=fields, lineterminator='\n')
            w.writeheader(); w.writerows(summaries)
        samples_path = PUBLIC / 'jpeg-decode-samples.csv'
        with samples_path.open('w', newline='') as f:
            fields = ['fixture','jpeg_side','quality','chroma','phase','sample','decode_us',
                      'image_count','encoded_bytes','rgb_fnv1a','exact']
            w = csv.DictWriter(f, fieldnames=fields, lineterminator='\n')
            w.writeheader(); w.writerows(all_samples)
        meta.update({'status':'complete', 'ended_utc':utcnow(), 'jobs_completed':len(summaries),
                     'host_load_end_top5':top_processes(),
                     'summary_sha256':sha256(summary_path), 'samples_sha256':sha256(samples_path)})
        method_path.write_text(json.dumps(meta, indent=2) + '\n')
    except Exception as e:
        meta.update({'status':'failed', 'ended_utc':utcnow(), 'error':repr(e),
                     'jobs_completed':len(summaries)})
        method_path.write_text(json.dumps(meta, indent=2) + '\n')
        raise


if __name__ == '__main__':
    main()
