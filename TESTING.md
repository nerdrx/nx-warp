# Testing NX Warp

A video codec has an unusually sharp definition of correct: **two independent
implementations must produce identical bytes**. Almost everything in this
document exists to make that statement checkable rather than aspirational.

The reference codec in `ref/` is the normative specification (`docs/SYNTAX.md` is
that specification in prose). The Vulkan encoder and decoder in `vk/` are
conformant when they reproduce the reference's output exactly — not "within a
tolerance", exactly. PAPER.md 3.9 calls the cross-vendor determinism test "the
definition of done for Phase 1 and Phase 2", and it means it.

Everything runs through **ctest**, and every test is named `<component>.<name>`
so a component's whole suite is one regex:

```sh
ctest --test-dir build -R '^ref\.'          # the reference codec
ctest --test-dir build -R '^vk\.'           # everything Vulkan
ctest --test-dir build -R '^examples\.'     # the examples still work
ctest --test-dir build --output-on-failure  # all of it
```

---

## 1. The pyramid

Seven layers, cheapest and narrowest first. A bug should be caught by the
highest-numbered layer that can catch it, and each layer exists because the one
above it is too slow, too vague, or needs hardware.

| # | Layer | Answers | Cost | Where |
|---|---|---|---|---|
| 1 | **Unit tests per component** | does this piece do what its header says | ms | `tests/<comp>/`, `ctest -R '^<comp>\.'` |
| 2 | **CPU↔GPU bit-exact diff** | does the shader agree with the reference, tile by tile | seconds to minutes | `tests/vk-decoder/`, `tests/warp/`, `tests/vk-encoder/` |
| 3 | **Conformance vectors** | does *any* decoder agree with the pinned bitstreams | ms | `tests/vectors/`, `ref.vectors` |
| 4 | **Fuzzing** | does malformed input ever crash or hang | hours | `-DNXVC_FUZZ=ON`, nightly |
| 5 | **Transport loss simulation** | does the client's state ever diverge from the encoder's shadow | seconds | `transport.*`, `examples.loopback_*`, `nxvc-netsim` |
| 6 | **Quality harness vs anchors** | is it *good*, in dB and BD-rate, against x264/x265 | minutes to hours | `tools/quality/`, `nxwarp-quality` |
| 7 | **Device benchmarks** | is it *fast enough*, on the hardware that matters | minutes | `bench/`, Pico 4 via adb |

Layers 1–3 and 5 are pass/fail and belong in CI. Layers 6 and 7 produce
**numbers**, and a number is only a test when it is compared against a threshold
the paper states — which is why `compare.py` prints PASS/FAIL against PAPER.md
3.11's own figures rather than leaving a chart for someone to squint at.

---

## 2. Running each layer

### Build first

```sh
chrt -i 0 taskset -c 4-7 nice -n 19 cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
chrt -i 0 taskset -c 4-7 nice -n 19 cmake --build build -j4
```

Every component is `ON` by default and guarded on its directory existing, so a
partial tree configures. Two switches matter for testing:

| Option | Default | Effect |
|---|---|---|
| `NXWARP_BUILD_TESTS` | `ON` | without it there is no test configuration at all, and `ctest` has nothing to read |
| `NXWARP_BUILD_VK` | `OFF` | the Vulkan components and every `vk.*` test. Needs Vulkan headers; a loader is optional (without one you get a compile-only build and the tools skip) |
| `NXWARP_BUILD_EXAMPLES` | `ON` | `examples/` and the three `examples.*` tests |
| `NXVC_FUZZ` | `OFF` | the libFuzzer targets; needs clang |
| `NXWARP_QUALITY_PYTHON` | *(found on PATH)* | the interpreter the Python quality suite runs under |

Build directories live **under the repository** (`build/`, `build-examples/`, …)
and are git-ignored. Large data goes on the scratch volume, never in the repo and
never in `/tmp` (tmpfs here).

### Layer 1 — unit tests

```sh
ctest --test-dir build -R '^ref\.'       # transform, rans, codec, headers, fuzz_smoke, cli, vectors
ctest --test-dir build -R '^warp\.'      # identity, interior, border, corners, mv, divide, int64, filters, oracle
ctest --test-dir build -R '^stereo\.'    # raster, disparity, determinism, sim_smoke
ctest --test-dir build -R '^transport\.' # wire, fec, packetizer, shadow, receiver
ctest --test-dir build -R '^rc\.'        # classify, foveation, allocate, model, governor
ctest --test-dir build -R '^hybrid\.'
```

The interesting ones are not the obvious ones:

* `ref.codec` checks rate/quality **monotonicity**, lossless bit-exactness, every
  tool combination, odd picture sizes and multi-frame streams. Monotonicity is
  the property that catches a quantiser bug no single-QP test can see.
* `ref.headers` checks TLV **forward compatibility**: an unknown extension must
  be skipped, not refused. A Phase 1 decoder handed a Phase 2 stream must fail
  with `NXVC_ERR_UNSUPPORTED`, cleanly, rather than misparse it.
* `warp.int64` and `warp.divide` assert what the *normative arithmetic forbids*:
  no 64-bit intermediates, no division. Those are not style rules — they are the
  reason the GPU and the CPU can agree bit for bit.
* `ref.fuzz_smoke` is a fast random/mutated-stream pass that runs on every
  commit; layer 4 is the long version of the same property.

### Layer 2 — CPU versus GPU, bit exact

This is the layer the whole design rests on. The shader and the reference are
driven with identical input and their outputs are compared byte for byte; the
harness reports the first mismatching tile and pixel, because "some tiles differ"
is not a bug report.

```sh
cmake -B build -G Ninja -DNXWARP_BUILD_VK=ON
ctest --test-dir build -R '^vk\.passA\.'      # entropy decode
ctest --test-dir build -R '^vk\.passB\.'      # reconstruction
ctest --test-dir build -R '^vk\.encoder\.'    # encoder statistics pass
ctest --test-dir build -R '^warp\.gpu_diff'   # the pose-warp kernel
```

`vk.passA` is registered several times over on purpose —
`gpu_roundtrip`, `gpu_ballot`, `gpu_lds_fallback`, `gpu_subgroup32`,
`gpu_subgroup64` — because the cluster-of-8 scheme has to give the same answer at
every subgroup width and through both the ballot and the LDS fallback path. A
kernel that is only correct at the width of your development GPU is a kernel
that fails on Adreno.

`vk.probe_lavapipe_is_pure` deserves its own mention: it asserts that the
lavapipe device really does expose the pure-compute path, so a green
`vk.passA.gpu_*` on CI is not quietly a hybrid fallback.

### Layer 3 — conformance vectors

`tests/vectors/` holds 32 committed `.nxv` bitstreams and `vectors.md5`, which
pins **two** hashes per vector: the MD5 of the bitstream and the MD5 of its
decoded planes.

```sh
ctest --test-dir build -R '^ref\.vectors'
build/tests/ref/nxv-vectors --check tests/vectors     # the same thing directly
```

> **A Vulkan decoder is conformant when it reproduces every `decoded_md5` in
> `tests/vectors/vectors.md5`.** That sentence is the whole conformance
> definition. Nothing else in this repository is allowed to contradict it.

Regenerating the vectors means the **bitstream changed**:

```sh
build/tests/ref/nxv-vectors --generate tests/vectors
```

That is a deliberate act. It must come with a `docs/SYNTAX.md` diff, and it must
be its own commit — see §4.

### Layer 4 — fuzzing

```sh
cmake -S . -B build-fuzz -G Ninja -DNXVC_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz -j4
chrt -i 0 taskset -c 4-7 nice -n 19 \
  build-fuzz/tests/ref/nxvc_fuzz_decode -max_len=4096 $NXQ_SCRATCH/fuzz-corpus/
```

The target is built with `-fsanitize=fuzzer,address,undefined` and compiles the
codec sources **into** the fuzzer rather than linking the static archive, so the
instrumentation actually reaches the decode path.

The property is not "does not crash". It is: **never reads out of bounds, and
always emits a frame or a clean error.** A decoder that survives by looping
forever has failed; the Phase 1 exit criterion is a 24-hour clean corpus run, and
timeouts count as bugs.

Seed the corpus from `tests/vectors/*.nxv` — structured input finds structural
bugs much faster than random bytes. The GPU decoder is fuzzed with the same
corpus under `VK_LAYER_KHRONOS_validation` with GPU-assisted validation on
lavapipe.

### Layer 5 — transport loss

```sh
ctest --test-dir build -R '^transport\.'
ctest --test-dir build -R '^examples\.loopback'
build/bin/nxvc-netsim --help      # the full simulator: multipath, bursts, FEC sweeps
build/bin/nxvc-example-loopback --cols 68 --rows 34 --loss 0.05 --burst 3 --paths 2
```

The property under test is **shadow correctness**: the encoder's model of what
the client has must never disagree with what the client actually has. PAPER.md
2.11 item 4 calls a divergence "a permanent artefact until the next refresh",
which is to say it is silent, persistent, and invisible to PSNR on the frame
where it happens.

`nxvc-example-loopback` makes that its exit code, which is why
`examples.loopback_clean` and `examples.loopback_lossy` are real tests and not
demos. `nxvc-netsim` is the long-running version that PAPER 2.11 item 4 asks to
run "for hours with zero mismatches before Phase 3".

### Layer 6 — quality against the anchors

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export NXW_CORPUS=$NXQ_SCRATCH/corpus
python3 corpus/fetch.py --sync                        # material (see corpus/README.md)

cd tools/quality
python3 compare.py --probe                            # what can this machine do?
python3 compare.py --seq $NXW_CORPUS/vr-mixed-256.yuv444p.json \
    --codec-cmd ../../build/bin/nxv --anchors x264-intra,x265-p \
    --qp 16,22,28,34 --anchor-qp 16,22,28,34 \
    --out $NXQ_SCRATCH/results/run.json
python3 report.py --results $NXQ_SCRATCH/results/run.json
```

The pytest suite behind it is a ctest target:

```sh
cmake -B build -DNXWARP_QUALITY_PYTHON=$NXQ_SCRATCH/venv/bin/python
ctest --test-dir build -R nxwarp-quality
```

Registration is **skipped with a status line, not failed**, when the interpreter
lacks pytest or numpy. Full documentation is `tools/quality/README.md`; §5 below
covers how to read what it produces.

For a two-second sanity check with no anchors and no Python at all, the examples
do the job:

```sh
build/bin/nxvc-example-roundtrip --in $NXW_CORPUS/vr-mixed-256.yuv444p.yuv \
    --w 512 --h 256 --pix yuv444p --qp 26
```

### Layer 7 — device benchmarks

`bench/` is the Phase 0 gate app (kernels K1–K6) and stays in the tree as the
regression benchmark:

```sh
bench/run-host.sh                      # desktop, writes results-host.json
bench/run.sh                           # Android via adb
python3 bench/report.py bench/results-host.json
```

The nightly Pico 4 runner fails if Pass B p99 regresses by more than 5 %.
`bench/README.md` records every decision the paper left open, including why each
kernel exists.

---

## 3. What "skipped" means

**A skipped test returns exit code 77** and ctest shows it as `Skipped`, not
`Passed` and not `Failed`. Every test that can be unrunnable sets
`SKIP_RETURN_CODE 77`.

This is a deliberate rule, and it matters more than it looks. A machine without a
Vulkan ICD is not a broken machine. Neither is one without numpy, or without a
corpus, or without a Pico 4 on the end of an adb cable. If those situations
produced red, everyone would learn to ignore red, and the day a real failure
appeared nobody would look.

So:

| Situation | Result |
|---|---|
| no Vulkan ICD | `vk.*` skip |
| ICD present but the device lacks the required subgroup features | that device's tests skip |
| a hybrid-only device with no pure-compute path | `vk.subgroup_semantics` skips |
| no lavapipe manifest found at configure time | the lavapipe-pinned tests are **never registered** (a different thing from skipping: they do not exist in this build) |
| no pytest or numpy | `nxwarp-quality` is not registered; a status line says why |
| no corpus materialised | `corpus/verify.py` returns 77 |
| no `nxvc_ref` in the build | `examples.smoke` is not registered |

If you want a run where skips are failures — which is the right stance for a
release gate — check the count instead:

```sh
ctest --test-dir build --output-on-failure --no-tests=error
ctest --test-dir build -N | awk '/Total Tests:/ {print $3}'
```

CI counts the registered tests before running them and says so in the log. It
currently *tolerates* a zero count (a tree with no `tests/` yet is a legitimate
state during the build-out) — which means a misconfigured build that silently
registers nothing passes. Once every component has landed, that tolerance should
become a floor; until then, read the "running N tests" line rather than trusting
the green tick.

---

## 4. Adding a conformance vector

A conformance vector is a committed bitstream that every decoder must reproduce
exactly. Add one when you add a **syntax element or a tool combination that no
existing vector exercises** — not when you add a feature that happens to use
existing syntax.

The 32 current vectors cover QP extremes, 4:2:0 and 4:4:4, lossless with and
without alpha, transform skip, res-level cycling, per-tile QP and res maps, 4:2:0
tiles inside a 4:4:4 picture, YCoCg-R, custom tables, every `nsub_log2`, all four
quantisation matrices, odd picture sizes, a single-tile picture, a one-row
picture, and a multi-frame stream. Read `tests/ref/vectors.cpp` before adding:
your case may already be there under a different name.

1. **Add the generator case** in `tests/ref/vectors.cpp`, next to the existing
   ones. Name it `vNN_<what_it_exercises>` — the name is documentation, and
   `v33_intra_dc_extremes` tells a future reader more than `v33_test2`.
2. **Make it exercise an extreme, not an average.** PAPER 3.9 lists what these
   are for: max-magnitude coefficients, max displacement, all-skip frames,
   DC-plane extremes, lossless tiles, truncated tiles. A vector that codes a
   pleasant gradient at QP 28 tests nothing that `v01` does not.
3. **Keep it small.** These are committed to git. Existing vectors are tens of
   kilobytes; a 64x64 or 200x140 picture is usually enough to hit the case.
4. Regenerate and check:
   ```sh
   cmake --build build -j4 --target nxv-vectors
   build/tests/ref/nxv-vectors --generate tests/vectors
   ctest --test-dir build -R '^ref\.vectors'
   ```
5. **Inspect what you just pinned** before committing it:
   ```sh
   build/bin/nxvc-example-tilewalk --in tests/vectors/v33_....nxv
   ```
   If the tile table does not show the thing you meant to exercise, the vector is
   wrong and you are about to pin the wrong bytes into the repository forever.
6. Commit `tests/vectors/v33_*.nxv`, the updated `vectors.md5`, and the
   `vectors.cpp` change **together**, and nothing else.

### Regenerating existing vectors

`--generate` rewrites every vector, not just yours. If hashes other than your new
one changed, **the bitstream format changed**. That is either a bug you just
introduced or a deliberate format change, and the two are told apart by whether
`docs/SYNTAX.md` changes with it:

* accidental → fix the code, do not commit the new hashes;
* deliberate → its own commit, with the `docs/SYNTAX.md` diff, and a message that
  says which syntax element moved. Every conformance claim and every quality
  number measured before that commit is void.

The same rule applies to `corpus/MANIFEST.json` hashes (see `corpus/README.md`)
and for the same reason.

---

## 5. Running on lavapipe, RADV and Android

PAPER 3.9 names three device classes, and the codec is only correct if all three
produce identical bytes.

### lavapipe (software, no GPU, what CI uses)

Mesa's software rasteriser. Its subgroup size is **8**, "which is exactly why the
cluster size is 8" — lavapipe is not a poor substitute for a GPU here, it is the
narrowest case the design has to handle.

```sh
sudo pacman -S vulkan-swrast          # Arch; mesa-vulkan-drivers on Debian
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
export VK_DRIVER_FILES=$VK_ICD_FILENAMES
ctest --test-dir build -R '^vk\.'
```

`tests/vk/CMakeLists.txt` also looks for a lavapipe manifest at **configure**
time and registers a second, ICD-pinned copy of each test
(`vk.probe_lavapipe`, `vk.subgroup_semantics_lavapipe`, …). That way a developer
on a GPU box still exercises the 8-lane path without changing their environment.
Point it at a manifest explicitly with:

```sh
cmake -B build -DNXVC_LAVAPIPE_ICD=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
```

Both `VK_ICD_FILENAMES` (legacy) and `VK_DRIVER_FILES` (current) are set, because
which one a loader honours depends on its version. No display is ever opened:
the tools are headless by construction, and an X or Wayland connection attempt on
a CI box is a hang rather than an error.

### RADV (AMD, the desktop development target)

The default on this machine; just do not force an ICD:

```sh
env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES ctest --test-dir build -R '^vk\.'
```

RADV's subgroup size is 64 by default and 32 under `RADV_PERFTEST=wave32`. Run
both — `vk.passA.gpu_subgroup32` and `vk.passA.gpu_subgroup64` exist precisely
because a kernel can be right at one width and wrong at the other.

Under `-DNXWARP_BUILD_VK=ON` the encoder is expected to come in under 4 ms on an
RX 580 (PAPER 3.11), which `vk.encoder.stats.*` measures rather than assumes.

### Android / Adreno (Pico 4, the target that decides the design)

There is no ctest here; the device is driven over adb.

```sh
bench/run.sh                # builds, installs, runs, pulls the JSON back
adb logcat -s nxbench
```

The nightly self-hosted runner is what makes this a test rather than an
experiment: it runs the conformance vectors on the device, compares hashes
against `tests/vectors/vectors.md5`, and fails on any mismatch or on a Pass B p99
regression above 5 %.

Two Adreno-specific traps the tests exist to catch, both from PAPER 3.12: the
proprietary compiler may spill or serialise the ballot-based scheme (hence
`vk.passA.gpu_lds_fallback`, the alternative path), and thermal throttling can
turn a 5 ms decoder into a 7 ms one over a session — so a single-shot benchmark
number from a cold device is not a result.

### Cross-vendor determinism

The test that closes Phase 1 and Phase 2 is not any single one of the above. It
is: **encode on AMD, decode on NVIDIA, lavapipe and Adreno, and get hashes equal
to the reference decoder.** Run the conformance vectors on each device and diff
the `decoded_md5` column. Anything else is a proxy for this.

---

## 6. Reading the quality reports

`compare.py` writes a JSON result file; `report.py` turns one or more of them
into Markdown plus an SVG rate-distortion plot in `tools/quality/reports/`.
Worked examples are committed there
(`example-dummy-1024.md`, `phase1-baseline-ref-intra.md`).

### The verdict lines

The harness evaluates the paper's criteria in code. A pass looks like:

```
Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
  PASS: worst -0.412 dB at 168.3 Mbit/s, mean -0.298 dB over 105.0-380.0 Mbit/s
```

and a non-verdict looks like:

```
  not evaluated: the 100-400 Mbit band is not covered by both curves
  (anchor spans 24.1-69.5, codec spans 14.6-43.4 Mbit/s); choose QP points
  that land in the band
```

**"Not evaluated" is not "nearly passed".** It is the harness refusing to invent
a number, and it is the single most common outcome on small test clips, because a
512x256 sequence at 90 Hz simply cannot produce 100 Mbit. The fix is bigger
material or a wider QP ladder, never a wider `--phase1-band`.

### What to look at, in order

1. **The gate verdict**, if the run covers the band.
2. **BD-rate and BD-PSNR.** They need overlap on *different* axes — BD-rate needs
   the quality ranges to overlap, BD-PSNR the rate ranges — so one can be
   computable when the other is not. Each is attempted and reported
   independently; a missing one is not a failure.
3. **The angular-velocity split**, present whenever the sequence has a pose log.
   PAPER 2.11 item 1 asks for BD-rate overall *and* on the 20 % highest-velocity
   frames, with different thresholds: within 10 % at rest, at least 30 % better
   on the motion frames. A codec that averages well and loses on the motion
   subset has failed the test the paper actually set.
4. **The RD curve shape.** A curve that flattens at high rate means something is
   saturating; a curve that crosses the anchor means the comparison is
   QP-dependent and a single-point claim from it would be dishonest.

### Numbers to distrust

* **PSNR on foveated content.** PAPER 5.3 opens by explaining why PSNR is the
  wrong tool for VR: it weights every pixel equally when 80 % are peripheral at
  ¼ sampling *by design*. Use `foveated_metrics.py` for anything foveated, and
  read its note about tan-projection pixel density before choosing `--ppd`.
* **A mean of per-frame dB.** Always wrong; average in the MSE domain. The
  harness and `roundtrip_psnr` both do, deliberately.
* **VMAF anywhere but the base layer.** It is trained on 4:2:0 camera content at
  television viewing distances. PAPER 5.3 keeps it "only as a sanity number for
  the base layer when compared with HEVC" — treat it as such.
* **Anything measured on synthetic material as a phase verdict.** The Phase 1 and
  Phase 2 criteria say *VR captures*. Synthetic sequences prove the pipeline is
  wired up correctly; captures decide the phase. See `corpus/README.md`.
* **`dummy_codec.py` output.** It has no transform, no prediction and no entropy
  model. It produces a monotone RD curve, which is enough to prove the plumbing
  and nothing else.

---

## 7. CPU discipline

Every heavy process — ffmpeg, x264, x265, the codec CLIs, the corpus generator,
the builds — runs at idle priority on a fixed core slice so it never competes
with the compositor or an interactive session:

```
chrt -i 0 taskset -c <cpus> nice -n 19 <cmd>
```

`tools/quality/nxq/cpu.py` applies it automatically (override the slice with
`NXQ_CPUS`, ffmpeg's thread count with `NXQ_THREADS`, disable it entirely with
`NXQ_NO_CPU_LIMIT=1` for CI containers that lack `chrt`/`taskset`).
`corpus/fetch.py` reuses that same helper. Apply it by hand to builds and to
long ctest runs.

---

## 8. Quick reference

```sh
# everything that can run here, with failures shown
ctest --test-dir build --output-on-failure

# one component
ctest --test-dir build -R '^ref\.' -V

# one test, verbose, with its output
ctest --test-dir build -R '^ref\.codec$' --output-on-failure -V

# what would run, without running it
ctest --test-dir build -N

# only what failed last time
ctest --test-dir build --rerun-failed --output-on-failure

# in parallel, on the idle slice
chrt -i 0 taskset -c 4-7 nice -n 19 ctest --test-dir build -j4 --timeout 600
```

| I want to know… | Run |
|---|---|
| did I break the bitstream | `ctest -R '^ref\.vectors'` |
| did I break the reference codec | `ctest -R '^ref\.'` |
| does the shader still match the reference | `ctest -R '^vk\.'` (needs `-DNXWARP_BUILD_VK=ON`) |
| does the API still work | `ctest -R '^examples\.'` |
| what is actually in this stream | `nxvc-example-tilewalk --in x.nxv` |
| did quality change | `nxvc-example-roundtrip --in seq.yuv --w W --h H --qp N` |
| is it good enough for Phase 1 | `tools/quality/compare.py` on a **VR capture** |
| does loss break it | `nxvc-example-loopback --loss 0.05 --burst 3` |
| is it fast enough | `bench/run.sh` on the Pico 4 |

See also: `examples/README.md` (what each example proves), `corpus/README.md`
(the test material and its provenance), `tools/quality/README.md` (the harness in
full), `ref/README.md` (the reference codec and the vector format),
`CONTRIBUTING.md` (style, commits, CI).
