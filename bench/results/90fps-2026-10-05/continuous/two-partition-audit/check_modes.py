#!/usr/bin/env python3
"""Execute the supplied Arm ASTC parser; no image data or production changes."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if len(sys.argv) != 3:
    sys.exit("usage: check_modes.py ASTC_SOURCE_DIRECTORY ASTC_STATIC_LIBRARY")
source, library = Path(sys.argv[1]), Path(sys.argv[2])
parser = source / "astcenc_block_sizes.cpp"
original = parser.read_text()
start = original.index("static bool decode_block_mode_2d(")
end = original.index("\n/**", start)
# Extracted function remains covered by Arm's Apache-2.0 source license.
program = '#include "astcenc_internal.h"\n#include <cassert>\n#include <cstdio>\n'
program += original[start:end]
program += r"""
int main() {
    struct Expected { unsigned mode,x,y,q,bits; bool valid; };
    for (auto e : {Expected{0x0E1,5,5,0,25,true}, {0x1BF,3,3,5,27,true},
                   {0x141,6,4,0,24,true}, {0x1AE,3,3,2,18,false}}) {
        unsigned x=0,y=0,q=0,bits=0; bool dual=false;
        bool valid=decode_block_mode_2d(e.mode,x,y,dual,q,bits);
        assert(valid==e.valid && x==e.x && y==e.y && q==e.q && bits==e.bits && !dual);
        std::printf("%03x valid=%d grid=%ux%u quant=%u weight_bits=%u\n",e.mode,valid,x,y,q,bits);
    }
}
"""
flags = ["-O2", "-std=c++17", "-mavx2", "-mpopcnt", "-mf16c", "-ffp-contract=off"]
flags += ["-DASTCENC_" + value for value in
          ["AVX=2", "F16C=1", "NEON=0", "POPCNT=1", "SSE=41", "SVE=0", "X86_GATHERS=1"]]
with tempfile.TemporaryDirectory(prefix="nx-astc-mode-check-") as temp:
    cpp, binary = Path(temp)/"check.cpp", Path(temp)/"check"
    cpp.write_text(program)
    subprocess.run([os.environ.get("CXX", "g++"), *flags, "-I"+str(source), str(cpp),
                    str(library), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
for path in [parser, library, Path(__file__)]:
    print(hashlib.sha256(path.read_bytes()).hexdigest(), path, flush=True)
