# Production UDP receive-buffer reuse probe

## Result

On a pinned desktop CPU, the bounded receive-buffer pool reduced matching
approximately 40 KiB allocations from a median of **376 to 16 per 400 batches**
(**95.7% fewer**). Median batch receive/drain p50 was 1.965 µs for the baseline
and 1.940 µs for the pool; p95 was 2.560 µs for both. The probe found no
measurable receive-path regression in this loopback workload.

This is a Linux PC loopback result using the production `wivrn::UDP` API. It
makes no FPS, Pico, headset, or real-network performance claim.

## Protocol

The 40 process runs used ten ABBA cycles (`baseline, pool, pool, baseline`),
with 20 runs per treatment. Each run sent 400 batches, each containing 20 UDP
datagrams of 1,400 bytes, and retained 16 batches at a time. The receiver used
`receive_raw()` followed by 19 `receive_pending()` calls for each batch. Byte
markers and retained packet contents were checked after each 16-batch block,
outside the timer.

The receive timer covered those 20 receive calls only. It excluded datagram
sends, process startup, and payload/lifetime validation. The 40 KiB allocation
counter was a global `operator new` size filter (40,960 through 41,216 bytes),
enabled only while those receive calls ran. The production allocation request
was 40,960 bytes; the observed `allocate_shared` allocation size was 41,007
bytes. Every run counted 376 requests for baseline and 16 for pool.

`runs.csv` records each run and links its raw stdout file in `runs/`. For each
run, p50 and p95 are computed over its 400 timed batch calls; plotted values are
the 20 run-level percentiles per treatment. The allocation count is constant
within each treatment across all 20 runs.

## Host and source identity

- Host CPU: AMD Ryzen 9 9950X3D, 32 logical CPUs.
- Both executables were pinned to logical CPU 20 with `taskset -c 20`. Compiler: GNU C++ (GCC) version is recorded below.
- CPU 20 measured 100% idle over a 0.5 s pre-run `/proc/stat` sample. Its
  governor was `performance`; an instantaneous frequency read was 5.46 GHz.
  Frequency was not locked throughout the run.
- Both versions of `common/wivrn_sockets.cpp` and the shared harness were
  compiled as C++20 with `-O2 -D_GNU_SOURCE`. They linked against the same
  checkout's CMake-built common libraries and crypto/Vulkan/system libraries.
- Baseline source and header come from WiVRn checkout commit
  `4d7a8dd5e08cc26e6a3b5668510db7c95d7978ca`. The pool source/header are the
  tested versions committed as `27026bbd9b2a3703b7acdffc6e49b88f81b9ab35` and recorded in `SOURCE_SHA256.txt`.

The benchmark translation units used `g++ (GCC) 16.2.1` (GNU toolchain); both were directly compiled with the flags below. The earlier Debug-pool-versus-`-O2` timing run was an optimization-level
mismatch and is discarded. It is not included in the CSV or plot.

## Reproduction

Use the source checkout at
`/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe` (or a matching
checkout with the recorded CMake dependencies). The repository and generated
build dependencies are intentionally referenced rather than copied here.

From that checkout, set `D` to this report directory and reconstruct the
baseline source snapshot from its recorded commit:

```sh
D=/run/media/nerdrx/Lex/claude/nx-warp/bench/results/90fps-2026-10-04/overnight-recovery/udp-reuse
BASE=/tmp/udp-reuse-baseline
mkdir -p "$BASE"
git show 4d7a8dd5e08cc26e6a3b5668510db7c95d7978ca:common/wivrn_sockets.cpp > "$BASE/wivrn_sockets.cpp"
git show 4d7a8dd5e08cc26e6a3b5668510db7c95d7978ca:common/wivrn_sockets.h > "$BASE/wivrn_sockets.h"
```

Compile the baseline object and harness with the baseline header first on the
include path. Compile the pool object and harness against the current source
and header. For both, use these flags and link libraries:

```sh
CXXFLAGS='-std=c++20 -O2 -D_GNU_SOURCE -Icommon -Ibuild-server/common -I. -Ibuild-server/_deps/boost-src/libs/pfr/include -Iexternal -I/run/media/nerdrx/Lex/claude/tools/local/include'
LIBS='build-server/common/libwivrn-common.a build-server/common/libwivrn-common-base.a -lcrypto -lvulkan -lpthread'
```

Build both measured translation units in each treatment with those same flags.
Put the baseline include directory FIRST to keep the harness and socket object
ABI identical. The baseline include path selects the matching baseline header; the pool uses
this checkout's production source and header:

```sh
c++ -I"$BASE" $CXXFLAGS -c "$BASE/wivrn_sockets.cpp" -o /tmp/udp-baseline-sockets.o
c++ -I"$BASE" $CXXFLAGS -c "$D/bench.cpp" -o /tmp/udp-baseline-bench.o
c++ /tmp/udp-baseline-bench.o /tmp/udp-baseline-sockets.o $LIBS -o /tmp/udp-baseline

c++ $CXXFLAGS -c common/wivrn_sockets.cpp -o /tmp/udp-pool-sockets.o
c++ $CXXFLAGS -c "$D/bench.cpp" -o /tmp/udp-pool-bench.o
c++ /tmp/udp-pool-bench.o /tmp/udp-pool-sockets.o $LIBS -o /tmp/udp-pool
```

Run ten ABBA cycles; process startup is outside the receive timers. The runner
writes a CSV and 40 stdout captures into its output directory. Python with
Matplotlib is needed to render the figure:

```sh
python3 "$D/run_abba.py" /tmp/udp-baseline /tmp/udp-pool "$D/reproduced" --cpu 20 --cycles 10
python3 "$D/plot.py" "$D/reproduced/runs.csv" "$D/reproduced/udp-reuse.png"
```

`plot.py` defaults to this report's `runs.csv` and `udp-reuse.png`; pass a CSV and output path to plot a reproduction run.
