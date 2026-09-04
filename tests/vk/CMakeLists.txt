# tests/vk - ctest entries for the shared Vulkan runtime.
#
# Every test here needs a Vulkan ICD.  A CI box or a developer machine without
# one must SKIP, not fail: both tools return 77 in that case and each test sets
# SKIP_RETURN_CODE 77.  docs/PAPER.md 3.9 names lavapipe and SwiftShader as the
# GPU-less CI targets, and lavapipe specifically because its subgroup size is 8
# -- "which is exactly why the cluster size is 8".
#
# Tests are registered against:
#   * whatever ICD the environment selects (CI exports VK_ICD_FILENAMES and
#     VK_DRIVER_FILES to lavapipe), and
#   * a lavapipe manifest found at configure time, if there is one, so a
#     developer on a GPU box still exercises the 8-lane path.

if(NOT TARGET nxvc-vkprobe OR NOT TARGET nxvc-vksubgroup)
  message(STATUS "tests/vk: vk tools not built, no Vulkan tests registered")
  return()
endif()

# ---------------------------------------------------------------- lavapipe
# Mesa's software rasteriser installs its manifest as lvp_icd.<arch>.json.
# The Android SDK emulator also ships one, which is what makes these tests
# runnable on a machine with no vulkan-swrast package installed.
set(_lvp_globs
  /usr/share/vulkan/icd.d/lvp_icd.*.json
  /usr/local/share/vulkan/icd.d/lvp_icd.*.json
  $ENV{HOME}/.local/share/vulkan/icd.d/lvp_icd.*.json
)
if(DEFINED ENV{ANDROID_SDK_ROOT})
  list(APPEND _lvp_globs $ENV{ANDROID_SDK_ROOT}/emulator/lib64/vulkan/lvp_icd.json)
endif()
if(DEFINED ENV{ANDROID_HOME})
  list(APPEND _lvp_globs $ENV{ANDROID_HOME}/emulator/lib64/vulkan/lvp_icd.json)
endif()
if(NXVC_LAVAPIPE_ICD)
  set(_lvp "${NXVC_LAVAPIPE_ICD}")
else()
  # list(GET) on an empty list is a hard error that aborts the whole project
  # configure, so never call it without checking first.
  set(_lvp "")
  file(GLOB _lvp_found ${_lvp_globs})
  if(_lvp_found)
    list(GET _lvp_found 0 _lvp)
  endif()
endif()
if(_lvp AND EXISTS "${_lvp}")
  message(STATUS "tests/vk: lavapipe ICD ${_lvp}")
else()
  set(_lvp "")
  message(STATUS "tests/vk: no lavapipe ICD found; lavapipe-pinned tests skipped. "
                 "Pass -DNXVC_LAVAPIPE_ICD=/path/to/lvp_icd.json to add them.")
endif()

# nxvc_vk_add_test(<name> <target> [ICD <manifest>] [ARGS ...])
function(nxvc_vk_add_test name target)
  cmake_parse_arguments(A "" "ICD" "ARGS" ${ARGN})
  add_test(NAME ${name} COMMAND $<TARGET_FILE:${target}> ${A_ARGS})
  set(env "")
  if(A_ICD)
    # Both spellings: VK_ICD_FILENAMES is the legacy name every loader still
    # honours, VK_DRIVER_FILES the current one.
    list(APPEND env "VK_ICD_FILENAMES=${A_ICD}" "VK_DRIVER_FILES=${A_ICD}")
  endif()
  # No display anywhere in this suite: the tools are headless by construction
  # and an X/Wayland connection attempt on a CI box is a hang, not an error.
  list(APPEND env "VK_LOADER_DISABLE_INST_EXT_FILTER=0")
  set_tests_properties(${name} PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 300
    LABELS "vk"
    ENVIRONMENT "${env}")
endfunction()

# ------------------------------------------------------------------- probe
# 3.7 in executable form.  Passes on any device; skips when there is no ICD.
nxvc_vk_add_test(vk.probe nxvc-vkprobe ARGS --json)

# ------------------------------------------------- subgroup semantics (3.2.6)
# The cluster-of-8 ballot/shuffle emulation must agree with a CPU reference, be
# identical at every pinnable subgroup size, and agree with the real
# subgroupClustered* ops where the device has them.  Skips (77) on a
# hybrid-only device, which has no pure-compute path to verify.
nxvc_vk_add_test(vk.subgroup_semantics nxvc-vksubgroup ARGS --verbose)

# --------------------------------------------------------- runtime self-test
# Creates a real device and exercises every helper the encoder and decoder
# lean on: buffers of each kind, an image and its layout transition, one-shot
# and pre-recorded command buffers, a timeline semaphore signalled from the
# host and from the queue, and a timestamp pool around a real dispatch.  Runs
# on hybrid-only devices too -- none of it is pure-compute specific.
nxvc_vk_add_test(vk.runtime_selftest nxvc-vkprobe ARGS --selftest)

# ----------------------------------------------------- lavapipe-pinned copies
if(_lvp)
  nxvc_vk_add_test(vk.probe_lavapipe nxvc-vkprobe ICD "${_lvp}" ARGS --json)
  # The 8-lane case: lavapipe's subgroup size is 8, the narrowest width the
  # codec claims to support, so this is the test that actually exercises the
  # boundary the cluster width was chosen for.
  nxvc_vk_add_test(vk.subgroup_semantics_lavapipe nxvc-vksubgroup
                   ICD "${_lvp}" ARGS --verbose)
  # A lavapipe device must reach at least the Lite profile, or the CI story of
  # 3.9 ("runs on lavapipe ... without a GPU") is not true any more.
  nxvc_vk_add_test(vk.probe_lavapipe_is_pure nxvc-vkprobe
                   ICD "${_lvp}" ARGS --quiet --require lite)
  nxvc_vk_add_test(vk.runtime_selftest_lavapipe nxvc-vkprobe
                   ICD "${_lvp}" ARGS --selftest)
endif()
