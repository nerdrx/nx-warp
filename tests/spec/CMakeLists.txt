# Specification consistency check.
#
# The root CMakeLists.txt adds every tests/<name>/CMakeLists.txt that exists,
# so this file is all the wiring the spec/ directory needs; spec/ itself has no
# CMakeLists.txt because it builds nothing.
#
# spec.check reports: it lists the [pending] markers and cross-checks clause 04
# against clause 05, and it passes while pending markers remain, because they
# are the honest state of a specification whose component documents are still
# landing. A release build would run it with --strict, which fails on them.

find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOT Python3_Interpreter_FOUND)
  message(STATUS "tests/spec: python3 not found, skipping spec.check")
  return()
endif()

set(NXWARP_SPEC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../spec)
set(NXWARP_SPEC_CHECK ${NXWARP_SPEC_DIR}/tools/check_spec.py)

if(NOT EXISTS ${NXWARP_SPEC_CHECK})
  message(STATUS "tests/spec: ${NXWARP_SPEC_CHECK} not present, skipping spec.check")
  return()
endif()

add_test(NAME spec.check
         COMMAND ${Python3_EXECUTABLE} ${NXWARP_SPEC_CHECK} ${NXWARP_SPEC_DIR})
set_tests_properties(spec.check PROPERTIES LABELS "spec;docs")

# The same check with --strict, for a release gate. It fails today by design:
# pending markers remain (52 at the time of writing). Disabled by default;
# enable with -DNXWARP_SPEC_STRICT=ON to see what a finished document set
# would have to satisfy.
option(NXWARP_SPEC_STRICT "Register spec.check.strict, which fails while [pending] markers remain" OFF)

if(NXWARP_SPEC_STRICT)
  add_test(NAME spec.check.strict
           COMMAND ${Python3_EXECUTABLE} ${NXWARP_SPEC_CHECK} --strict --quiet ${NXWARP_SPEC_DIR})
  set_tests_properties(spec.check.strict PROPERTIES LABELS "spec;docs")
endif()
