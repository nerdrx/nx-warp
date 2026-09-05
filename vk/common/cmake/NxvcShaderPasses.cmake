# NxvcShaderPasses.cmake -- the one spirv-opt pass list every NX Warp shader is
# optimised with, and the one place it is written down.
#
# Includable twice over: from a normal CMakeLists at configure time, and from a
# `cmake -P` build-time script (see nxvc_gen_spv.cmake).  It defines variables
# only; it creates no targets and reads no project state.
#
# ---------------------------------------------------------------------------
# Why this is not just `glslc -O` / `spirv-opt -O`
# ---------------------------------------------------------------------------
# This is spirv-opt's own -O list with BOTH redundancy-elimination steps
# removed:
#
#     --redundancy-elimination
#     --local-redundancy-elimination
#
# Redundancy elimination common-subexpression-eliminates the duplicate
# OpAccessChain that a load and a store to the same shared-memory word each
# produce, so both end up using one pointer id.  The result is valid SPIR-V --
# spirv-val accepts it, and RADV (wave32 and wave64) and lavapipe are bit-exact
# on it -- but the Adreno 650 driver on the Pico 4 miscompiles Pass B's LDS
# transpose when it is fed that form: the word that comes back out of shared
# memory belongs to a different slot, while every input to it (the coefficient
# word, the dequantised coefficient, the row-pass result and both the store and
# the load address) is verified bit-exact by --selftest.  Dropping this one pass
# is what makes the Phase 0 gate bit-exact on the target device; the driver's
# own compiler does its own redundancy elimination, so nothing is given up.
#
# Bisected pass by pass on device; see bench/README.md "Adreno and spirv-opt"
# and docs/ADRENO-RULES.md.
#
# `glslc -O` runs the full built-in list, redundancy elimination included, and
# so must never be used on a shader that reaches a device.  Compile with -O0 and
# run spirv-opt separately with NXVC_SPIRV_SAFE_PASSES.  scripts/shader-lint.py
# enforces that (rule `spv-unsafe-opt`).

include_guard(GLOBAL)

set(NXVC_SPIRV_SAFE_PASSES
  --wrap-opkill --eliminate-dead-branches --merge-return
  --inline-entry-points-exhaustive --eliminate-dead-functions
  --eliminate-dead-code-aggressive --private-to-local
  --eliminate-local-single-block --eliminate-local-single-store
  --eliminate-dead-code-aggressive --scalar-replacement=0
  --convert-local-access-chains --eliminate-local-single-block
  --eliminate-local-single-store --eliminate-dead-code-aggressive
  --ssa-rewrite --eliminate-dead-code-aggressive --ccp
  --eliminate-dead-code-aggressive --loop-unroll --eliminate-dead-branches
  --combine-access-chains --simplify-instructions --scalar-replacement=0
  --convert-local-access-chains --eliminate-local-single-block
  --eliminate-local-single-store --eliminate-dead-code-aggressive
  --ssa-rewrite --eliminate-dead-code-aggressive --vector-dce
  --eliminate-dead-inserts --eliminate-dead-branches --simplify-instructions
  --if-conversion --copy-propagate-arrays --reduce-load-size
  --eliminate-dead-code-aggressive --merge-blocks
  --eliminate-dead-branches --merge-blocks --simplify-instructions
  CACHE INTERNAL "spirv-opt passes that are safe on the Adreno 650 driver")

# paper 3.10 asks for SPIR-V 1.4.  That needs Vulkan 1.2 or VK_KHR_spirv_1_4 on
# a 1.1 device, and the Pico 4's Adreno driver is a plain Vulkan 1.1
# implementation, so the default is vulkan1.1 (SPIR-V 1.3), which every device
# in the 3.7 table accepts.
if(NOT DEFINED NXVC_SPIRV_TARGET_ENV)
  set(NXVC_SPIRV_TARGET_ENV "vulkan1.1")
endif()

# The build-time worker script, so a caller can name it without knowing where
# this module lives.
set(NXVC_GEN_SPV_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/nxvc_gen_spv.cmake"
    CACHE INTERNAL "cmake -P script that compiles one shader to SPIR-V")
