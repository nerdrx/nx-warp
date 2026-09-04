# Response to the external review of 2026-09-05

An external reviewer read the tree at `8322708` and reported seven findings,
the first of them being that `main`'s CI is red. It was. This document answers
each finding in order, says plainly whether the project agrees with it, and
names the commit that acted on it or the issue that carries what is left.

Every finding is accepted in substance. Three carry a correction of detail, and
in two of those the reality is worse than the finding said, not better. One
defect the review did not name was found while fixing the ones it did; it is
recorded in section 8 rather than left out because nobody asked about it.

The review's own framing — that a project whose central claim is bit-exact
cross-vendor determinism cannot ship a CI that has been red for days — is
correct and is not argued with anywhere below.

---

## 1. Only 77 of 79 ctests pass on the Linux job

**Partly agree — the count is right and the diagnosis under it is not, and CI
was redder than 77 of 79.**

The two failures were `nxwarp-quality` and `hybrid.e2e256`. Neither is a broken
test. Both are the same two environment defects the review reports separately
as findings 2 and 3, arriving through the test suite:

- `hybrid.e2e256` shells out to ffmpeg under `chrt -i 0 taskset -c 12-15 nice
  -n 19`. CPU 12 does not exist on a four-core GitHub runner, `taskset` exits
  non-zero, and `subprocess.run(check=True)` turns that into a
  `CalledProcessError` out of `hybrid/sim/nxvchybrid/cpu.py`.
- `nxwarp-quality` is the `pytest` run of the quality harness. Three of its
  tests failed: `test_bdrate.py` on `np.trapezoid`, and `test_codec.py` and
  `test_compare.py` on the same `taskset` failure, this time from
  `tools/quality/nxq/cpu.py`'s slice of `28-31`.

So neither test needed to be marked as a skip, and neither has been. Marking
them `77` would have hidden two real, reproducible portability bugs behind a
green tick — which is the outcome this project should least want. Both now pass
because the cause is fixed; see sections 2 and 3.

The correction to the finding is that 77 of 79 understates how red `main` was.
On the same push, three of six CI jobs and one of three Sanitizers jobs failed:

| job | why |
|---|---|
| `linux-gcc` | the two ctests above |
| `linux-clang` | the same two |
| `python` | four `pytest` failures — the two `taskset` ones, plus a corpus byte-pin failure the ctest jobs did not hit (section 8) |
| `windows-mingw` | finding 5, at configure time |
| `fuzz-smoke` | finding 4, at build time |
| `docs` (separate workflow) | finding 6 — failing on **every** push since the key was added, not just this one |

Local verification after the fixes, on the development host under
`chrt -i 0 taskset -c 16-19 nice -n 19`:

```
ctest --test-dir build-ci -j4          75 registered, 74 pass, 1 skipped
                                       (vk.passB.gpu_roundtrip_lavapipe: no lavapipe ICD here)
tools/quality: pytest -q              356 passed
```

The local run registers 75 tests rather than CI's 79 because CI has a lavapipe
ICD and this host does not, so some lavapipe-pinned Vulkan tests are not
registered here and the one that is, skips. Nothing fails.

## 2. Fixed CPU affinity refers to CPUs that do not exist on GitHub runners

**Agree, and it was in more places than the finding names.**

Fixed in `d076b53`.

The tree had three independent fixed slices, all correct on the 32-core
development host and all nonsense elsewhere: `28-31` in the quality harness,
`12-15` in the hybrid simulator, `20-23` in the bench scripts. Each guarded
itself with `shutil.which("taskset")` or `command -v taskset`, which answers
"is the tool installed", a question that was never the problem.

The rule now is: ask whether the CPUs exist, not whether the tool does.

- Python (`tools/quality/nxq/cpu.py`, `hybrid/sim/nxvchybrid/cpu.py`): the
  requested slice is parsed and intersected with `os.sched_getaffinity(0)`. An
  empty intersection drops `taskset` from the prefix and keeps `chrt` and
  `nice`, which are what actually keep a build out of the compositor's way.
  `chrt`, `taskset` and `nice` are each probed independently, so a machine with
  some of them gets what it has.
- Shell: one implementation, `scripts/cpu-discipline.sh`, sourced by
  `bench/run.sh`, `bench/run-host.sh`, `bench/gen-asset.sh` and
  `platform/win/build.sh`. It probes by running `taskset -c "$spec" true`.
  `fuzz/tools/run_campaign.sh` already did exactly this and is what the rest
  was modelled on; the finding is really that the good pattern existed and had
  not been applied anywhere else.

`scripts/build-all.sh` needed no change: it never used `taskset`.

Verified on this host, where a 32-core machine can be made to look like a
four-core one by running the probe under a restricted mask:

```
NXQ_CPUS=28-31, under taskset -c 16-19  ->  chrt -i 0 nice -n 19        (no pinning)
NXQ_CPUS=16-19, under taskset -c 16-19  ->  chrt -i 0 taskset -c 16-19 nice -n 19
nx_cpu_prefix 200-203                   ->  chrt -i 0 nice -n 19        (no pinning)
```

The environment overrides are unchanged and still documented: `NXQ_CPUS`,
`NXVCH_CPUS`, `NX_CPUS`, and `NXQ_NO_CPU_LIMIT` / `NXVCH_NO_CPU_LIMIT` /
`NX_NO_CPU_LIMIT` to switch the prefix off entirely.

## 3. `pyproject` declares `numpy>=1.22` but the code uses `np.trapezoid`

**Agree on the defect. Partly disagree on where it lives — the `pyproject` the
finding points at is not the one that governs the failing code.**

Fixed in `ba89aa4`.

`python/pyproject.toml` with its `numpy>=1.22` is the `nxvc` Python *bindings*
package. It does not use `trapezoid`, and its floor is accurate. The failing
code is the quality harness under `tools/quality/`, which is not a package at
all and, before this change, declared no dependencies anywhere — CI fell
through to a bare `pip install numpy pytest`.

The remedy is both halves of what the finding proposes, because neither alone
is enough:

- `tools/quality/tests/test_bdrate.py` binds the function once at import:
  `_trapz = getattr(np, "trapezoid", None) or np.trapz`. A version pin cannot
  fix this on its own, because CI legitimately runs two numpy majors — the C++
  jobs use the distribution's `python3-numpy` (1.26, which has only `trapz`)
  and the `python` job pip-installs current numpy (2.5, which no longer has
  `trapz` *at all*, so hard-coding either name breaks one job or the other).
- `tools/quality/requirements.txt` now states the harness's floor
  (`numpy>=1.22`, `pytest>=7.0`) explicitly. The `python` workflow already
  preferred a `requirements.txt` when one existed, so no workflow change was
  needed.

The harness is now written to run on both majors rather than to demand one.

## 4. A fuzz target cannot find `nxvc/warp.h`

**Agree on the defect. Correcting the location: it is not in
`fuzz/CMakeLists.txt`.**

Fixed in `1a42e34`.

`fuzz/CMakeLists.txt` is correct as it stands — it takes its include
directories from the component library targets it links. The failing target is
`nxvc_fuzz_decode`, defined in `tests/ref/CMakeLists.txt`, which deliberately
compiles the `ref/src` sources *into* the fuzz binary so libFuzzer sees
instrumented codec code rather than a plain static archive. That source list
had not followed `ref/` into Phase 2:

- `ref/src/codec.cpp` now includes `inter.h`;
- `inter.h` includes `<nxvc/warp.h>`, which lives in `warp/include`;
- that directory reaches a target only through `nxvc_warp_ref`'s `PUBLIC`
  include directory, and this target did not link it.

The fix adds `ref/src/inter.cpp` to the source list and links `nxvc_warp_ref`,
which supplies both the header and the predictor's definitions. The underlying
fault is that a hand-maintained source list was duplicating
`ref/CMakeLists.txt` and drifted from it; the duplication is now flagged in a
comment, and deduplicating it properly is worth doing but is not done here.

Verified locally with the exact `fuzz-smoke` configuration: the build completes
and all seven `*_replay` regression runners pass their checked-in reproducers.

## 5. The MinGW cross build pulls in `platform/win` and demands Vulkan headers

**Agree.**

Fixed in `1a42e34`.

`platform/CMakeLists.txt` gated `win/` on `WIN32` alone. Cross-compiling for
Windows sets `WIN32`, so the mingw job walked into `platform/win`, which is the
Vulkan-to-D3D11 external-memory interop probe and calls `message(FATAL_ERROR)`
when it cannot find `vulkan/vulkan.h` — on a runner that has no Vulkan SDK and
was explicitly configured with `NXWARP_BUILD_VK=OFF`.

Of the two remedies the finding offers, gating was chosen over dropping
`platform` from the preset's components, because the CI job does not use the
preset — it configures by hand — so a preset change would have left the job
red. `win/` is now skipped whenever `NXWARP_BUILD_VK=OFF`, which covers the
preset and the hand-rolled job together, and prints why. A Windows target with
Vulkan on still builds the probe, which is the configuration the probe is for.

Verified locally end to end with llvm-mingw: `cmake --preset mingw-w64`
configures (printing `NXWARP_BUILD_VK=OFF, skipping win/`) and builds all 84
targets clean. CI uses Debian's `g++-mingw-w64-x86-64` rather than llvm-mingw,
so the compiler is not the one exercised here; the gate being tested is
compiler-independent, but that job going green is confirmed by the next run.

## 6. MkDocs fails on null `custom_dir` / `logo` / `favicon`

**Agree. It was failing on every push, not intermittently.**

Fixed in `9f7eab6`.

MkDocs runs `os.path.isabs()` over `theme.custom_dir`, so `custom_dir: null` is
a `TypeError` during config validation rather than a "no overrides" — the whole
config fails to load before anything is built. There is no override directory,
so the key is gone rather than emptied. `logo` and `favicon` now point at real
files: `docs/assets/logo.svg` and `docs/assets/favicon.svg`, copied from
`brand/` because MkDocs resolves theme assets against `docs_dir` and will not
reach above it. `brand/README.md` remains the source of truth for the marks.

Verified in the venv at `nx-scratch/nx-warp/venv` with `mkdocs-material` 9.x:
`mkdocs build` — the workflow's exact command — succeeds, and both assets land
in `site/assets/`.

`mkdocs build --strict` still fails, for a separate and deliberate reason that
this change does not touch. Root-level documents (`ROADMAP.md`, `SECURITY.md`,
`CONTRIBUTING.md`, `GOVERNANCE.md`, component READMEs) live outside `docs_dir`
and are linked to GitHub rather than copied in, so there is exactly one copy of
each; `--strict` counts all 52 of those as broken links. That is why
`mkdocs.yml` sets `strict: false` and the workflow does not pass `--strict`.
Making the site strict-clean means deciding whether those documents move into
`docs/` — a real decision, not a config fix, and one this response does not
make. There is no issue for it yet.

## 7. Documentation drift

**Agree that all four claims were false. Correcting two of the replacement
numbers, and the ROADMAP one is worse than the finding says.**

Fixed in `b046d05`, with `76ef89f` for the missing file.

**"There are no CMake presets yet."** `CMakePresets.json` has twelve visible
configure presets (`dev`, `dev-vk`, `release`, `release-lto`, `asan-ubsan`,
`tsan`, `gcc`, `clang`, `coverage`, `fuzz`, `mingw-w64`, `android-ndk`) over
four hidden base presets. The README's Building section now lists them and
leads with `cmake --preset dev && cmake --build --preset dev && ctest --preset
dev`.

**"Twelve conformance vectors."** Correcting the finding's replacement figure:
the counts are **56 decode vectors and 29 rejection vectors**, not 61 and 32.
The authority is the two manifests, `tests/vectors/vectors.md5` and
`tests/vectors/rejects.md5`, excluding their comment headers — 59 and 34 lines
respectively, of which 3 and 5 are comments. Counting `.nxv` files gives 85,
which matches. `ref.vectors` checks every one. Both the status table and the
"good first contributions" paragraph now say 56.

**"No numerical Phase 1 measurements exist."** This is the one worth stating
carefully, because the correction runs against the project's interest. The
claim was not merely stale — its replacement is bad news:

- The Phase 1 intra criterion is *within 1.0 dB of x264 intra*. It has been
  measured: **+61.4 percent BD-rate against `x264 --keyint 1 --tune
  zerolatency`** at 4:4:4 on `vr-mixed-1024-v2`, a mean deficit of **3.72 dB**.
- Both Phase 2 kill-test bands fail on both sequences, at **+206 to +548
  percent**.
- The warp chain gate produces **zero frames above 35 dB**.
- `tools/quality/reports/gates-v2-2026-09-04.md` carries ten gate verdicts and
  ten FAILs, re-run on band-limited v2 material that moved the source by 12 dB
  without moving a single verdict. `ref/RESULTS-intra.md`,
  `ref/RESULTS-inter.md` and `ref/RESULTS-sparse.md` are the long form.

The ROADMAP now says this in the banner and in the Phase 1 and Phase 2 status
cells, in those words. "No measurements exist" was the comfortable claim; the
true one is worse, so the true one is in the file. This is the same standard
`docs/PAPER.md` 7.2 sets for itself ("what it does not claim") and that
`ROADMAP.md`'s own "How this file stays honest" section already demanded — the
file had simply stopped meeting it. Closing the intra gap is
[issue #39](https://github.com/nerdrx/nx-warp/issues/39); the parity kill test
is [issue #14](https://github.com/nerdrx/nx-warp/issues/14).

**`bench/results/` is not in the tree.** It was in the working tree and
gitignored, so the citation resolved to nothing for anyone who cloned. The one
run that is actually cited is now published at
`bench/results/results-host.json` (`76ef89f`): RADV 7900 XTX, 2048x4096, 600
frames after 120 warmup, K1 to K5 PASS, K6 skipped. `bench/.gitignore` keeps
every other local run ignored, so a run becomes a record only by being named.
This changes nothing about the Phase 0 gate itself, which is the on-device
table and does not exist — see [issue #44](https://github.com/nerdrx/nx-warp/issues/44)
for the CI half of that.

**The tournament.** Neither file said anything about where the compression work
actually is, which is a larger omission than any of the four. Both now record
it: thirteen `tourney/*` branches, each an independently built coding-tool
package; five judge reports covering the five paired packages (transform, intra
detail, entropy contexts, inter prediction, encoder RDO), each judged on
identical material by a third party against `TOURNEY-RULES.md`'s criteria in
order. Two branches (`tourney/sparse`, `tourney/metric`) are merged into
`main`; the other eleven are judged or recorded and **not merged**, and that
merge is the next piece of work. The ROADMAP names each package, its branches,
its judge report and its verdict.

It also states two things a reader would otherwise have to discover: the
branches and the judge reports are local and unpushed, so a clone has neither;
and several branches measured themselves with a harness bug that inflated codec
bitrates threefold on truncated sequences (fixed on `main` in `7576021`), so a
branch's own published BD-rate is not comparable with anything until a judge
re-measured it. No tournament claim appears in the README status table until it
lands on `main` and is re-measured there.

---

## 8. One defect the review did not name

Found while fixing finding 1, and recorded here because it was found, not
because it was asked about.

The `python` CI job failed on a fourth test the ctest jobs did not hit:
`test_synth_v2.py::TestLegacy::test_legacy_reproduces_the_published_corpus_bytes`,
a byte pin on the synthetic corpus generator. It failed with one wrong hash on
the `python` job and a *different* wrong hash on `linux-clang`, and passed on
the development host — three machines, three answers.

The cause is that `capture/synth.py` reached the BLAS. `dirs @ R.T` on an
(N, 3) float32 array dispatches to `sgemm`, and OpenBLAS selects its kernel per
micro-architecture, so the last bit of the render depends on which CPU the
runner happened to be. Every hash in `test_synth_v2.py` and in
`corpus/MANIFEST.json` is a byte pin on that arithmetic.

The matrices are 3x3. `b12c52f` writes the products out in elementwise
operations, which IEEE-754 defines exactly and numpy performs element by
element with no reassociation, removing the BLAS from the path. Verified
byte-neutral on this host: all fourteen synth pins, including
`CORPUS_PANEL_V1_SHA`, still hold bit for bit, so no published material changes
and no measurement has to be redone.

This one is a hypothesis confirmed only by the next CI run, because the
machines that disagreed are the CI runners. If it recurs, the next step is to
compare the generator's intermediate arrays across runners rather than the
final hash.

It is worth naming what this was: a determinism bug, in a project whose central
claim is bit-exact reproducibility across vendors, sitting in the tooling that
generates the material every quality number is measured on. It was invisible on
one machine.

---

## What is fixed, and what the next CI run has to confirm

Verified locally, under `chrt -i 0 taskset -c 16-19 nice -n 19`, `-j4`:

| finding | verification |
|---|---|
| 1, 2, 3 | `ctest` 74 of 75 pass, 1 skipped for a missing ICD; `pytest` 356 passed |
| 4 | the `fuzz-smoke` configuration builds; seven of seven replay runners pass |
| 5 | `cmake --preset mingw-w64` configures and builds 84/84 targets |
| 6 | `mkdocs build` succeeds; `logo.svg` and `favicon.svg` in `site/assets/` |
| 7 | every number re-derived from the tree and cited to the file it came from |

Confirmed only by the next run: the `windows-mingw` job (CI uses Debian's
`g++-mingw-w64`, not the llvm-mingw tested here), and the corpus determinism
fix in section 8 (the runners are the machines that disagreed).

Not addressed here, and no issue exists for either: making the docs site
strict-clean, and deduplicating `tests/ref/CMakeLists.txt`'s hand-maintained
copy of `ref/`'s source list, which is what let finding 4 happen.

## References

- `docs/PAPER.md` 7.2 (what the paper does not claim), 7.3 (roadmap),
  7.4 (build the benchmark first)
- `ROADMAP.md`, particularly "How this file stays honest"
- `tools/quality/reports/gates-v2-2026-09-04.md`, `ref/RESULTS-intra.md`,
  `ref/RESULTS-inter.md`
- `TESTING.md` for the CPU discipline as it is now documented
