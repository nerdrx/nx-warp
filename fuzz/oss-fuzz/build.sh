#!/bin/bash -eu
# OSS-Fuzz build script for NX Warp.
#
# Status: skeleton.  It is correct as written and can be dropped into an
# OSS-Fuzz project directory as-is, but the project has not been submitted to
# OSS-Fuzz yet, so it is not exercised by anything in CI.  What is exercised is
# everything it depends on: the same targets, the same corpora and the same
# dictionaries are built and run by .github/workflows/sanitizers.yml and
# nightly.yml, so this file cannot drift far from something that works.
#
# OSS-Fuzz contract (see https://google.github.io/oss-fuzz/):
#   $SRC       checkout root, this repo at $SRC/nx-warp
#   $WORK      scratch build directory
#   $OUT       where fuzz targets, seed corpora, dictionaries and options go
#   $CC/$CXX, $CFLAGS/$CXXFLAGS   the engine's instrumentation flags
#   $LIB_FUZZING_ENGINE           what a target links for its main()
#
# Notes for whoever submits this:
#   * ENGINES: libfuzzer, afl, honggfuzz.  Every target uses
#     LLVMFuzzerTestOneInput and one LLVMFuzzerCustomMutator; the custom
#     mutator is a libFuzzer feature, so under afl/honggfuzz the targets still
#     work but mutate as plain byte streams.
#   * SANITIZERS: address, undefined, memory.  MSan needs an instrumented
#     libc++, which OSS-Fuzz provides; the codec has no uninitialised reads by
#     construction (every buffer is memset or fully written), so MSan is worth
#     having.
#   * The transport library optionally links OpenSSL.  It is switched off here
#     so the build has no external dependency; NullAead is what the transport
#     targets use anyway.

cd "$SRC/nx-warp"

# ---------------------------------------------------------------------------
# Build.  NXVC_FUZZ builds the targets; NXWARP_FUZZ is not used because
# OSS-Fuzz already puts the coverage instrumentation in $CFLAGS, and
# NXWARP_SANITIZER is left off for the same reason: $CFLAGS carries the
# sanitizer the engine asked for.
# ---------------------------------------------------------------------------
cmake -S . -B "$WORK/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$CFLAGS" \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    -DNXVC_FUZZ=ON \
    -DNXWARP_BUILD_VK=OFF \
    -DNXWARP_BUILD_TOOLS=OFF \
    -DNXWARP_BUILD_TESTS=OFF \
    -DNXVC_TRANSPORT_OPENSSL=OFF \
    -DNXVC_TRANSPORT_SODIUM=OFF

cmake --build "$WORK/build" --parallel "$(nproc)"

# ---------------------------------------------------------------------------
# Install targets, seed corpora and dictionaries.
# ---------------------------------------------------------------------------
TARGETS="
nxvc_decode_fuzz
nxvc_headers_fuzz
nxvc_rans_fuzz
transport_depacketize_fuzz
transport_rs_fuzz
transport_feedback_fuzz
warp_tile_fuzz
"

for t in $TARGETS; do
    bin=$(find "$WORK/build" -type f -perm -u+x -name "$t" | head -n1 || true)
    if [ -z "$bin" ]; then
        echo "warning: $t was not built (its component may not be present yet)"
        continue
    fi
    cp "$bin" "$OUT/$t"

    # Seed corpus: the checked-in seeds plus the permanent reproducers, zipped
    # as OSS-Fuzz expects.  Real conformance vectors are already in there.
    seeds="$WORK/seeds/$t"
    mkdir -p "$seeds"
    cp -r "fuzz/corpus/$t/." "$seeds/" 2>/dev/null || true
    cp -r "fuzz/regressions/$t/." "$seeds/" 2>/dev/null || true
    ( cd "$seeds" && zip -q -r "$OUT/${t}_seed_corpus.zip" . ) || true

    # Dictionary.
    case "$t" in
        nxvc_*)      cp fuzz/dict/nxvc.dict "$OUT/$t.dict" ;;
        transport_*) cp fuzz/dict/nxt.dict  "$OUT/$t.dict" ;;
        warp_*)      cp fuzz/dict/warp.dict "$OUT/$t.dict" ;;
    esac

    # Per-target options.  max_len matches what the harnesses bound themselves
    # to; a larger input only makes the decoder slower, never more interesting.
    cat > "$OUT/$t.options" <<OPTS
[libfuzzer]
max_len = 65536
timeout = 25
rss_limit_mb = 4096
OPTS
done

echo "built: $(ls "$OUT" | tr '\n' ' ')"
