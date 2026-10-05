#!/usr/bin/env bash
set -euo pipefail

repo=${1:?source checkout}
ndk=${2:?Android NDK}
out=${3:?scratch output directory}
here=$(cd "$(dirname "$0")" && pwd)
report="$here/../quiet-retirement-kernel"
cache="$repo/.cxx/RelWithDebInfo/33s4w1c5/arm64-v8a"
mkdir -p "$out"

python3 "$report/generate.py" "$repo" "$out"
cp "$out/kernel-gate.cpp" "$out/kernel-gate.generated.cpp"
python3 - "$out/kernel-gate.cpp" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()
needle = "namespace wivrn {\nnamespace application { struct config { bool shard_retransmit=true; }; inline config current{}; inline config& get_config(){return current;} }"
fixture = needle + "\nnamespace application { struct jni_fixture { void setup_jni() {} }; inline jni_fixture& instance(){ static jni_fixture value; return value; } }"
if s.count(needle) != 1:
    raise SystemExit("expected unique projected application fixture insertion point")
p.write_text(s.replace(needle, fixture, 1))
PY

cxx="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++"
"$cxx" -std=c++23 -DANDROID -O2 -g -static-libstdc++ -Wall -Wextra -Werror \
  -Wno-error=sign-compare -Wno-error=unused-parameter -Wno-error=missing-field-initializers \
  -pthread \
  -I"$repo/client/decoder" -I"$repo/common" -I"$cache/common" -I"$repo" \
  -I"$repo/external" -I"$cache/_deps/boost-src/libs/pfr/include" \
  -I"$cache/_deps/openssl/include" -I"$cache/_deps/spdlog-src/include" \
  -I"$cache/_deps/openxr_loader-src/include" \
  "$out/kernel-gate.cpp" "$repo/common/wivrn_sockets.cpp" \
  "$repo/common/crypto.cpp" "$repo/common/smp.cpp" \
  "$cache/_deps/openssl/lib/libcrypto.a" "$cache/_deps/spdlog-build/libspdlog.a" \
  -ldl -llog -o "$out/kernel-gate-android" >"$out/build-android.log" 2>&1

"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -h -d "$out/kernel-gate-android" >"$out/elf-inspection.txt"
sha256sum "$out/kernel-gate.generated.cpp" "$out/kernel-gate.cpp" \
  "$out/kernel-gate-android" "$repo/client/scenes/stream_network.cpp" \
  "$repo/client/decoder/shard_accumulator.cpp" "$repo/client/decoder/frame_window.h" \
  "$repo/client/decoder/nack_deadline.h" "$repo/common/wivrn_sockets.cpp" \
  "$repo/common/crypto.cpp" "$repo/common/smp.cpp" \
  "$cache/_deps/openssl/lib/libcrypto.a" "$cache/_deps/spdlog-build/libspdlog.a" \
  >"$out/hashes.txt"
