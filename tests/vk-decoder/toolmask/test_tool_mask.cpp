// vk.decoder.tool_mask -- what this decoder tells an encoder it can take.
//
// A capability mask is a promise.  The one bit this test exists for is
// XFORM_LARGE (27): on the Adreno 650 the 16x16 and 32x32 module decodes
// streams wrong, and on the 4:4:4 32x32 conformance vector
// (v70_xform32_444) it WEDGES -- a fence that never signals, no kgsl fault,
// no GPU reset, the sweep dead where it stands.  A decoder that advertises
// that bit on that device is inviting a conformant encoder to end the
// session, so tools_supported_for() clears it and this pins the behaviour.
//
// It links the parse unit only: no Vulkan, no device, no shaders, so it runs
// everywhere and answers in milliseconds.
//
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <cstdio>

#include "nxvc_vkdec_parse.h"

namespace {
int g_fail = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

constexpr uint64_t kXformLarge = 1ull << 27;
}  // namespace

int main() {
    const uint64_t build = nxvcvk::tools_supported();

    // The build implements it; that is not in question and is what makes the
    // per-device subtraction meaningful rather than vacuous.
    check((build & kXformLarge) != 0,
          "the build-wide mask offers XFORM_LARGE");

    // Qualcomm by vendor id, whatever the name says.
    check((nxvcvk::tools_supported_for(0x5143u, "Adreno (TM) 650") &
           kXformLarge) == 0,
          "Adreno by vendor id clears XFORM_LARGE");
    check((nxvcvk::tools_supported_for(0x5143u, nullptr) & kXformLarge) == 0,
          "Adreno by vendor id with no name clears XFORM_LARGE");

    // ...and by name, for a Qualcomm part behind a translation layer that
    // reports someone else's id.  Case-insensitively.
    check((nxvcvk::tools_supported_for(0x1002u, "Adreno (TM) 650") &
           kXformLarge) == 0,
          "Adreno by name clears XFORM_LARGE");
    check((nxvcvk::tools_supported_for(0x1002u, "adreno 740") & kXformLarge) ==
              0,
          "Adreno by lowercase name clears XFORM_LARGE");

    // Exactly one bit is removed, and only on that device.  A mask that
    // quietly dropped anything else would pass a "bit 27 is clear" test.
    check(nxvcvk::tools_supported_for(0x5143u, "Adreno (TM) 650") ==
              (build & ~kXformLarge),
          "Adreno clears XFORM_LARGE and nothing else");

    // Every other device keeps the whole mask.
    check(nxvcvk::tools_supported_for(0x1002u, "AMD Radeon RX 7900 XTX (RADV "
                                              "NAVI31)") == build,
          "RADV keeps the build-wide mask");
    check(nxvcvk::tools_supported_for(0x10005u, "llvmpipe (LLVM 19.1.0, 256 "
                                               "bits)") == build,
          "lavapipe keeps the build-wide mask");
    check(nxvcvk::tools_supported_for(0u, nullptr) == build,
          "an unknown device keeps the build-wide mask");

    std::printf(g_fail ? "-- %d failure(s)\n" : "-- tool mask ok\n", g_fail);
    return g_fail ? 1 : 0;
}
