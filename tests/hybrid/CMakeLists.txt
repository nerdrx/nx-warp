# tests/hybrid -- end-to-end checks for the hybrid-mode simulator.
#
# Every test skips (exit code 77) rather than fails when its prerequisites are
# absent, so a machine without ffmpeg/libx265 or without numpy still gets a
# green ctest run.

find_package(Python3 COMPONENTS Interpreter QUIET)

set(_hybrid_sim_dir "${CMAKE_SOURCE_DIR}/hybrid/sim")

if(NOT Python3_FOUND)
  add_test(NAME hybrid.python
           COMMAND "${CMAKE_COMMAND}" -E echo "SKIP: python3 not found")
  set_tests_properties(hybrid.python PROPERTIES SKIP_RETURN_CODE 77)
  return()
endif()

# A scratch directory outside the repo; large material must never land in the
# source tree (or in /tmp, which is a tmpfs here).
if(DEFINED ENV{NXVCH_SCRATCH})
  set(_hybrid_scratch "$ENV{NXVCH_SCRATCH}")
else()
  set(_hybrid_scratch "${CMAKE_CURRENT_BINARY_DIR}/hybrid-scratch")
endif()

# 1. Import and unit-level checks: no ffmpeg needed.
add_test(NAME hybrid.units
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_units.py")
set_tests_properties(hybrid.units PROPERTIES
  SKIP_RETURN_CODE 77
  ENVIRONMENT "PYTHONPATH=${_hybrid_sim_dir};NXVCH_SCRATCH=${_hybrid_scratch}"
  TIMEOUT 300)

# 1b. The spatial hybrid's geometry, composite and decoder cost model: numpy
#     only, no ffmpeg and no nxv-enc.
add_test(NAME hybrid.spatial_units
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_spatial_units.py")
set_tests_properties(hybrid.spatial_units PROPERTIES
  SKIP_RETURN_CODE 77
  ENVIRONMENT "PYTHONPATH=${_hybrid_sim_dir};NXVCH_SCRATCH=${_hybrid_scratch}"
  TIMEOUT 300)

# 2. The tiny 256^2 end-to-end run: material -> x265 base -> enhancement ->
#    metrics, for all three codec paths.  Skips without ffmpeg.
add_test(NAME hybrid.e2e256
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_e2e_256.py")
set_tests_properties(hybrid.e2e256 PROPERTIES
  SKIP_RETURN_CODE 77
  ENVIRONMENT "PYTHONPATH=${_hybrid_sim_dir};NXVCH_SCRATCH=${_hybrid_scratch}"
  TIMEOUT 900)
