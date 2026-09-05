# Shader portability lint.
#
# The root CMakeLists.txt adds every tests/<name>/CMakeLists.txt that exists, so
# this file is all the wiring scripts/shader-lint.py needs.
#
# lint.shaders reads the GLSL and the CMake shader-build rules; it compiles
# nothing and needs no GPU, so unlike the rest of tests/vk-* it can never skip.
# What it checks and why is docs/ADRENO-RULES.md; the short version is that the
# Adreno 650 lowers indexed local storage to private memory and was measured
# reading it back wrong, so these are correctness rules.
#
# It is a hard test on purpose. Every finding it reports today has either been
# fixed or carries an `nxvc-lint: allow <rule> -- <reason>` waiver in the source,
# so a new one means a new regression rather than a backlog item.

find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOT Python3_Interpreter_FOUND)
  message(STATUS "tests/lint: python3 not found, skipping lint.shaders")
  return()
endif()

set(NXWARP_SHADER_LINT ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/shader-lint.py)

if(NOT EXISTS ${NXWARP_SHADER_LINT})
  message(STATUS "tests/lint: ${NXWARP_SHADER_LINT} not present, skipping lint.shaders")
  return()
endif()

add_test(NAME lint.shaders
         COMMAND ${Python3_EXECUTABLE} ${NXWARP_SHADER_LINT}
                 --root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set_tests_properties(lint.shaders PROPERTIES LABELS "lint;shaders")

# The same check with --strict, which also fails on the advisory rule
# (loop-local-array-index: a local array indexed by a constant-bound loop
# counter, correct only for as long as the unroller reaches it). Disabled by
# default because Pass B still carries several while its rewrite lands; enable
# it to see what a tree with no indexed local storage at all would have to
# satisfy, and make it the default once Pass B is clear.
option(NXWARP_SHADER_LINT_STRICT
       "Register lint.shaders.strict, which fails on advisory findings too" OFF)

if(NXWARP_SHADER_LINT_STRICT)
  add_test(NAME lint.shaders.strict
           COMMAND ${Python3_EXECUTABLE} ${NXWARP_SHADER_LINT} --strict
                   --root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  set_tests_properties(lint.shaders.strict PROPERTIES LABELS "lint;shaders")
endif()
