# ASTC packet-vector recycle component probe

This bundle records a bounded client-side packet-buffer reuse experiment from
WiVRn source commit `b712179` (full source snapshot `221f6834`). The inline
clear/swap helper is the same source used by the decoder. It keeps at most one
idle packet vector per decoder, transfers ownership using `clear()` and
`swap()`, and rejects any spare whose actual vector capacity exceeds
`raw_bytes + 32` bytes. It does not change ASTC decoding or the CPU-scratch to
staging-buffer copy path.

The component test exercises the actual helper for empty and non-empty
assemblers, content-preserving rejection, larger-spare replacement, the
capacity bound, and a 10,000-iteration producer/worker handoff using one
`std::mutex` and condition variable. It passed under normal, ASan+UBSan, and
ThreadSanitizer builds. The Android arm64 `wivrn` native target also built
successfully with the production integration. The source tree contains no
full-decoder ownership or Vulkan integration test; the handoff test is
deliberately limited to the production helper and its lock discipline. No
headset execution was part of this report.

## Deterministic 270-fragment counts

The benchmark used two existing 2176×2176 ASTC LDR 8×8×1 q6 fixture files as
inputs. It verified each ASTC header and block-data length, compressed the
block bytes with Zstd level 3, checked the decompressed roundtrip, and made a
valid 24-byte NXASTC v2 header. The fixture files themselves are not included.

Each packet was split into 270 contiguous fragments. Both modes append the
same packet bytes and move the completed vector. The baseline destroys its
completed vector each frame. The candidate returns a worker-owned vector after
the next producer frame has already been assembled, then reuses it on the
following frame. Thus the first two candidate frames allocate; the remaining
998 reuse the spare.

| Fixture | NXASTC packet bytes | Baseline vector growths / relocated bytes | Recycle vector growths / relocated bytes |
|---|---:|---:|---:|
| dark q6 | 416,403 | 10,000 / 788,214,000 B | 20 / 1,576,428 B |
| forest q6 | 258,395 | 10,000 / 489,064,000 B | 20 / 978,128 B |

A vector-capacity growth is one allocation/reallocation for this
`std::vector<uint8_t>` implementation. “Relocated bytes” sums the vector's
previous size at each growth; it is a deterministic estimate of bytes moved by
reallocation, not a hardware memory-traffic counter. Both modes still append
all packet bytes, and the recycle path saves no payload append copies.

The log also contains timings, but this was a sequential, unpinned host
microbenchmark without interleaved A/B samples or captured host load. Treat
those timings as conditional component observations only; they are not a
latency win claim and say nothing about full Vulkan decoding, Pico, network
throughput, or headset behavior.

The retained idle spare is strictly bounded to one vector with capacity at
most `raw_bytes + 32`; for these fixtures that limit is 1,183,776 bytes per
stream. A vector grown beyond the limit is discarded rather than retained.
The component benchmark confirms steady reuse only when a returned worker
buffer is available by the next assembly boundary. Full production worker,
queue-drop, and teardown lifecycles remain unmeasured by this probe.

## Reproduce

Requires a C++23 compiler, Zstd development headers/library, and pthreads. The
source bundle preserves the include layout used by the test.

```sh
g++ -std=c++23 -O2 source/tests/astc_packet_buffer_recycle_test.cpp \
  -lzstd -pthread -o packet-recycle-test
./packet-recycle-test
```

To repeat the component benchmark, provide local 2176×2176 ASTC LDR 8×8×1 q6
files. No image fixtures are distributed in this bundle:

```sh
./packet-recycle-test /path/to/dark-q6.astc /path/to/forest-q6.astc
```

Sanitizer checks for the helper and threaded handoff:

```sh
g++ -std=c++23 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer source/tests/astc_packet_buffer_recycle_test.cpp \
  -lzstd -pthread -o packet-recycle-asan
./packet-recycle-asan

g++ -std=c++23 -O1 -g -fsanitize=thread \
  source/tests/astc_packet_buffer_recycle_test.cpp \
  -lzstd -pthread -o packet-recycle-tsan
./packet-recycle-tsan
```

Toolchain for the recorded benchmark: GCC 16.2.1 and Zstd 1.5.7. See
`benchmark-log.txt` for the raw console output and `SHA256SUMS.txt` for source
file hashes.

![Component evidence and limits](packet-recycle.png)
