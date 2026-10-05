#!/usr/bin/env bash
set -euo pipefail
out=${1:?output directory required}
mkdir -p "$out"
rtk proxy g++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers gpu-gate.cpp -lvulkan -o "$out/gpu-gate"
VK_LOADER_LAYERS_DISABLE="~implicit~" rtk proxy timeout 15 "$out/gpu-gate" > "$out/validation.log" 2>&1
sha256sum gpu-gate.cpp > "$out/source-sha256.txt"
