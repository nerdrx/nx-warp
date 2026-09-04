# tests/vk-encoder -- ctests for the GPU encoder passes.
#
# Every test that needs a GPU exits 77 when there is no ICD, no physical device,
# or a device that cannot meet the kernels' requirements (256-lane workgroups,
# subgroup width at least 8 per paper 3.2.6).  SKIP_RETURN_CODE turns that into
# a ctest skip rather than a failure, so the suite is green on a headless CI
# runner and on a developer box alike.

if(NOT TARGET nxvc-stats-test)
  message(STATUS "tests/vk-encoder: nxvc-stats-test not built, no tests added")
  return()
endif()

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

set_tests_properties(vk.encoder.stats.cpu
                     vk.encoder.stats.diff
                     vk.encoder.stats.diff.device1
                     vk.encoder.stats.diff.seed2
                     PROPERTIES TIMEOUT 600 LABELS "vk-encoder")
