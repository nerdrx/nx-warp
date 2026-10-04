"""Matched lossless packet CPU cost; ctypes, reused contexts, verify every output."""
from pathlib import Path
import ctypes as C
import ctypes.util
import hashlib
import json
import statistics
import time

ROOT = Path(__file__).parent
INPUT = ROOT.parent / 'astc-production-colour-port-20261004'
z = C.CDLL(ctypes.util.find_library('zstd'))
l = C.CDLL(ctypes.util.find_library('lz4'))
def bind(lib, name, result, args):
    f = getattr(lib, name); f.restype = result; f.argtypes = args; return f
ptr, size, integer = C.c_void_p, C.c_size_t, C.c_int
bound = bind(z, 'ZSTD_compressBound', size, [size])
create = bind(z, 'ZSTD_createCCtx', ptr, [])
release = bind(z, 'ZSTD_freeCCtx', size, [ptr])
pack = bind(z, 'ZSTD_compressCCtx', size, [ptr, ptr, size, ptr, size, integer])
unpack = bind(z, 'ZSTD_decompress', size, [ptr, size, ptr, size])
error = bind(z, 'ZSTD_isError', C.c_uint, [size])
lbound = bind(l, 'LZ4_compressBound', integer, [integer])
lpack = bind(l, 'LZ4_compress_default', integer, [ptr, ptr, integer, integer])
lunpack = bind(l, 'LZ4_decompress_safe', integer, [ptr, ptr, integer, integer])
ctx = create(); assert ctx
rows = []
try:
    for scene in ('dark', 'forest'):
        for q in (0, 2, 4, 6):
            raw = (INPUT / f'{scene}-production-q{q}.astc').read_bytes()[16:]
            n = len(raw); src = C.create_string_buffer(raw)
            dst = C.create_string_buffer(max(bound(n), lbound(n)))
            restored = C.create_string_buffer(n)
            for kind in ('lz4', 'zstd1', 'zstd3'):
                times, decode_times, packed_sizes = [], [], []
                for i in range(42):
                    start = time.perf_counter_ns()
                    count = lpack(src, dst, n, len(dst)) if kind == 'lz4' else pack(ctx, dst, len(dst), src, n, int(kind[-1]))
                    duration = (time.perf_counter_ns() - start) / 1e6
                    assert count > 0 and not error(count)
                    start = time.perf_counter_ns()
                    got = lunpack(dst, restored, count, n) if kind == 'lz4' else unpack(restored, n, dst, count)
                    decode_duration = (time.perf_counter_ns() - start) / 1e6
                    assert got == n and restored.raw == raw
                    if i >= 12:
                        times.append(duration); decode_times.append(decode_duration); packed_sizes.append(count)
                assert len(set(packed_sizes)) == 1
                rows.append(dict(scene=scene, q=q, compression=kind, raw_bytes=n, packed_bytes=packed_sizes[0],
                                 raw_sha256=hashlib.sha256(raw).hexdigest(), median_ms=statistics.median(times),
                                 p95_ms=sorted(times)[28], decode_median_ms=statistics.median(decode_times),
                                 samples_ms=times, decode_samples_ms=decode_times))
finally:
    release(ctx)
(ROOT / 'results.json').write_text(json.dumps(dict(method='Local CPU; reused Zstd CCtx; 12 warmups, 30 samples; full 1920x1080 ASTC payload, no header. Decoder correctness checked every sample. CPU decode is PC only, not Pico.', rows=rows), indent=2) + '\n')
for row in rows:
    print(row['scene'], row['q'], row['compression'], row['packed_bytes'], round(row['median_ms'], 3), round(row['p95_ms'], 3))
