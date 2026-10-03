#!/usr/bin/env bash
set -euo pipefail
repo=/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-sparse-clear-20261003
build="$repo/build-android-arm64"
deps=/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-atlas-live-client-build/_deps
shader="$repo/common/pyrowave/shaders"
out="$build/common/pyrowave"
for name in dwt_haar_0 dwt_haar_1 dwt_haar_2 idwt_haar; do
  glslangValidator -V --target-env vulkan1.1 -S comp -DCOMP_SHADER "$shader/$name.comp" -I"$shader" -o "$out/$name.spv-nopt"
  hexdump -ve '1/4 "0x%08X, "' "$out/$name.spv-nopt" > "$out/$name.spv"
done
if ! rg -q 'dwt_haar_0' "$out/pyrowave_shaders.cpp"; then
  python - "$out/pyrowave_shaders.cpp" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text()
items=''.join('{ "'+n+'", {\n#include "'+n+'.spv"\n}},\n' for n in ('dwt_haar_0','dwt_haar_1','dwt_haar_2','idwt_haar'))
p.write_text(s.replace('};}',items+'};}'))
PY
fi
common=(-O2 -DNDEBUG -std=c++23 -DVMA_VULKAN_VERSION=1003000 -DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS -DPYROWAVE_HAAR_FORMAT=1 -I"$repo/common/pyrowave" -I"$repo/common" -I"$build/common" -I"$build/common/pyrowave" -I"$repo" -I"$repo/external" -I"$build/Vulkan-Headers-vulkan-sdk-1.3.268.0/include" -I"$deps/boost-src/libs/pfr/include")
libs=("$repo/common/vk/allocation.cpp" "$repo/common/vk/vk_allocator.cpp" "$repo/common/vk/vk_mem_alloc.cpp" "$repo/common/vk/error_category.cpp")
c++ "${common[@]}" "$repo/encode-frame-host.cpp" "$repo/common/pyrowave/pyrowave_common.cpp" "$repo/common/pyrowave/pyrowave_encoder.cpp" "$out/pyrowave_shaders.cpp" "${libs[@]}" -lvulkan -o "$repo/pyrowave-haar-host-encoder"
c++ "${common[@]}" "$repo/pyrowave-haar-compute-readback.cpp" "$repo/common/pyrowave/pyrowave_common.cpp" "$repo/common/pyrowave/pyrowave_decoder.cpp" "$out/pyrowave_shaders.cpp" "${libs[@]}" -lvulkan -o "$repo/pyrowave-haar-host-decoder"
