# hybrid/ -- the hybrid-mode (HEVC base + pose-warped enhancement) experiment.
#
# There is nothing to compile: the simulator is Python + numpy (see
# hybrid/README.md for why).  This file exists so the root CMakeLists can
# add_subdirectory(hybrid) uniformly, and so `nxvc-hybridsim` is discoverable
# from a build tree.

find_package(Python3 COMPONENTS Interpreter QUIET)

set(NXWARP_HYBRIDSIM "${CMAKE_CURRENT_SOURCE_DIR}/sim/nxvc-hybridsim"
    CACHE INTERNAL "path to the hybrid-mode simulator")

if(NOT Python3_FOUND)
  message(STATUS "hybrid: python3 not found; nxvc-hybridsim will not be runnable")
endif()

add_custom_target(hybridsim-help
  COMMAND "${NXWARP_HYBRIDSIM}" --help
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sim"
  COMMENT "nxvc-hybridsim usage"
  VERBATIM)
