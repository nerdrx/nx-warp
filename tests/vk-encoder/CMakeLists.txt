# tests/vk-encoder -- ctests for the GPU encoder passes.
#
# Every test that needs a GPU exits 77 when there is no ICD, no physical device,
# or a device that cannot meet the kernels' requirements (256-lane workgroups,
# subgroup width at least 8 per paper 3.2.6).  SKIP_RETURN_CODE turns that into
# a ctest skip rather than a failure, so the suite is green on a headless CI
# runner and on a developer box alike.

set(NXENC_TESTS)

if(TARGET nxvc-stats-test)
  # ------------------------------------------------------- E0 / E1 / E2
  # CPU models only: no Vulkan, so this one can never be skipped.  It is the
  # guard that the bit-exact reference itself still behaves after an edit.
  add_test(NAME vk.encoder.stats.cpu
           COMMAND nxvc-stats-test --cpu-only)

  # GPU vs CPU diff on the default device.  This is the test paper 3.9 calls the
  # definition of done for these kernels.
  add_test(NAME vk.encoder.stats.diff
           COMMAND nxvc-stats-test --device 0 --no-bench)
  set_tests_properties(vk.encoder.stats.diff PROPERTIES SKIP_RETURN_CODE 77)

  # A second physical device if the box has one -- on this developer machine that
  # is the integrated GPU, and in CI it is usually nothing.  Running the same diff
  # on two devices at once is cheap and is how a subgroup-width dependency would
  # first show up locally.
  add_test(NAME vk.encoder.stats.diff.device1
           COMMAND nxvc-stats-test --device 1 --no-bench)
  set_tests_properties(vk.encoder.stats.diff.device1 PROPERTIES SKIP_RETURN_CODE 77)

  # Same kernels under a second seed, so a passing run is not an artefact of one
  # particular set of test frames.
  add_test(NAME vk.encoder.stats.diff.seed2
           COMMAND nxvc-stats-test --device 0 --no-bench --seed 987654321)
  set_tests_properties(vk.encoder.stats.diff.seed2 PROPERTIES SKIP_RETURN_CODE 77)

  list(APPEND NXENC_TESTS vk.encoder.stats.cpu vk.encoder.stats.diff
              vk.encoder.stats.diff.device1 vk.encoder.stats.diff.seed2)
else()
  message(STATUS "tests/vk-encoder: nxvc-stats-test not built, no E0/E1/E2 tests")
endif()

# ------------------------------------------------- the C header / GLSL mirror
# Needs no target and no GPU: it reads the two source files.  nxe_enc.h and
# nxe_enc_common.glsl are hand-written copies of one contract, and a
# disagreement between them is silent -- NXE_LANE_OPS_CAP is a buffer stride
# shared between the host and the shader, so a drift there makes every rANS
# lane address another lane's scratch and quietly writes a wrong stream.
add_test(NAME vk.encoder.mirror
         COMMAND ${CMAKE_COMMAND}
         -DCHDR=${CMAKE_CURRENT_SOURCE_DIR}/../../vk/encoder/forward/nxe_enc.h
         -DGHDR=${CMAKE_CURRENT_SOURCE_DIR}/../../vk/encoder/forward/nxe_enc_common.glsl
         -P ${CMAKE_CURRENT_SOURCE_DIR}/mirror.cmake)
list(APPEND NXENC_TESTS vk.encoder.mirror)

if(TARGET nxvc-vkenc)
  # -------------------------------------------------------- E3 / E4 / E5
  # The CPU models against their pinned stream digests.  No Vulkan, so it never
  # skips; it is what fails loudly if an edit changes the bitstream.
  add_test(NAME vk.encoder.forward.cpu
           COMMAND nxvc-vkenc --selftest --cpu)

  # The GPU pipeline against the CPU models: coefficients out of E3, then the
  # whole frame out of E4 and E5, over the same configuration table.
  add_test(NAME vk.encoder.forward.diff
           COMMAND nxvc-vkenc --selftest --device 0)
  set_tests_properties(vk.encoder.forward.diff PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME vk.encoder.forward.diff.device1
           COMMAND nxvc-vkenc --selftest --device 1)
  set_tests_properties(vk.encoder.forward.diff.device1 PROPERTIES SKIP_RETURN_CODE 77)

  list(APPEND NXENC_TESTS vk.encoder.forward.cpu vk.encoder.forward.diff
              vk.encoder.forward.diff.device1)

  # ------------------------------------------------------------ the acid test
  # A frame the GPU encoded must be byte-identical to the one the reference
  # encoder produces for the same decisions, and must decode with nxv-dec to
  # the same pixels.  Only available when the reference tools are in the build,
  # which they are from the repo root and are not in the standalone encoder
  # build; the digest test above is what covers the standalone case.
  if(TARGET nxv-enc AND TARGET nxv-dec)
    foreach(dev cpu 0)
      set(_name vk.encoder.acid.${dev})
      add_test(NAME ${_name} COMMAND ${CMAKE_COMMAND}
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-${dev}
               -DDEVICE=${dev}
               -P ${CMAKE_CURRENT_SOURCE_DIR}/acid.cmake)
      set_tests_properties(${_name} PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS ${_name})
    endforeach()
    # The same claim for the C ABI, which acid.cmake does not reach: the
    # library's own configuration mapping is what an integrator links.
    if(TARGET nxvc-vkenc-api)
      add_test(NAME vk.encoder.acid.api COMMAND ${CMAKE_COMMAND}
               -DAPIENC=$<TARGET_FILE:nxvc-vkenc-api>
               -DVKENC=$<TARGET_FILE:nxvc-vkenc>
               -DNXVENC=$<TARGET_FILE:nxv-enc>
               -DNXVDEC=$<TARGET_FILE:nxv-dec>
               -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/acid-api
               -P ${CMAKE_CURRENT_SOURCE_DIR}/api_acid.cmake)
      set_tests_properties(vk.encoder.acid.api PROPERTIES
                           SKIP_REGULAR_EXPRESSION "SKIP:")
      list(APPEND NXENC_TESTS vk.encoder.acid.api)
    endif()
  else()
    message(STATUS "tests/vk-encoder: nxv-enc/nxv-dec not built, no acid test")
  endif()
else()
  message(STATUS "tests/vk-encoder: nxvc-vkenc not built, no E3/E4/E5 tests")
endif()

if(NXENC_TESTS)
  set_tests_properties(${NXENC_TESTS} PROPERTIES TIMEOUT 900 LABELS "vk-encoder")
endif()
