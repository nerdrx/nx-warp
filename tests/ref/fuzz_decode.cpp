// libFuzzer entry point.  Build with -DNXVC_FUZZ=ON and a clang toolchain.
#include "fuzz_target.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return nxvc_fuzz_decode(data, size);
}
