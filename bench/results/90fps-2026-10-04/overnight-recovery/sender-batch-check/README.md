# UDP sender batching check

Scratch-only loopback check against the production `typed_socket<UDP,...>` API and current `wivrn_sockets.cpp`; no production files changed. Built the current socket/crypto source files directly because linking the older existing common archive caused a `UDP` destructor crash from a header/archive mismatch. Test uses real IPv6 loopback UDP and 1,200-byte payloads. AES-CTR enabled for order/payload checks and timings. Socket send/receive buffers are set to 4 MiB per socket to avoid local queue overflow at width 128. For timing, sender helper durations, including payload copies, serialization, AES and send syscalls, are included; receiver drain/decrypt happens between timed groups. Batch and individual paths each use fresh payload storage because production UDP encryption mutates serialized spans in place. 50 warmup groups per mode, then five matched `IBBI` sequences (10 blocks/mode, 200 groups/block) per width, pinned CPU 3 at nice +5. Means are microseconds/datagram, not network latency.

| Shards per group | Individual | `send_many_raw` | Difference |
|---:|---:|---:|---:|
| 1 | 1.046 | 1.051 | +0.5% |
| 8 | 1.092 | 0.990 | -9.3% |
| 64 | 1.057 | 0.972 | -8.0% |
| 128 | 1.057 | 0.973 | -7.9% |

The 0.07–0.10 us/datagram apparent savings are small and this loopback microbenchmark has no confidence interval. At 2,000 repair shards/sec they amount to at most roughly 0.2 ms CPU/sec at 0.1 us/shard. This alone does not justify repair-only batching.

Encrypted order and exact payload round-trip passed for widths 1, 8, 64 and 128. A linker-wrapped `sendmmsg` deterministically capped one eight-message call to one accepted message: the receiver got one datagram, zero remained queued, while `send_many_raw` returned all 9,728 expected serialized bytes. A separate deterministic wrapper returned `-1/EINTR`; `send_many_raw` threw EINTR. Both confirm production code does not retry/handle partial completion; the positive partial case silently drops the tail. Fixing that contract is prerequisite to using batching for repair replies.

## Reproduction

From `wt-pyrowave-probe` repository root:

```sh
g++ -std=c++23 -O2 -DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS \
  -I common -I /run/media/nerdrx/Lex/claude/nx-scratch/wivrn-valclean-build/common \
  -isystem /run/media/nerdrx/Lex/claude/tools/local/include \
  -isystem /run/media/nerdrx/Lex/claude/nx-scratch/wivrn-valclean-build/_deps/boost-src/libs/pfr/include \
  -isystem external \
  "/path/to/report/sender-batch-check/check.cpp" \
  common/wivrn_sockets.cpp common/crypto.cpp common/smp.cpp \
  -Wl,--wrap=sendmmsg -lcrypto -pthread \
  -o "/tmp/nx-sender-batch-check"

taskset -c 3 nice -n 5 "/tmp/nx-sender-batch-check"
```

Limits: synthetic single-process loopback says nothing about Wi-Fi, queue pressure, repair deadlines, or peer CPU. No actual sender/pacer path or retransmission scheduling was exercised. The forced partial test validates current syscall handling deterministically; it does not reproduce kernel partial sends under pressure.

The frame/shard IDs in this serialization test cycle across 64-shard synthetic frames; there is no view/final timing or complete-frame delivery gate. This is a socket/helper check, not a native encoder send benchmark. Actual live encryption configuration is not altered.
