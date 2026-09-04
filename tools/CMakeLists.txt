# tools/ -- host-side tooling.
#
# The quality harness is pure Python; nothing here builds C++. This file only
# registers the harness's pytest suite with ctest, so `ctest` runs it alongside
# the codec's own tests.
#
#   ctest -R nxwarp-quality            run the harness tests
#   ctest -R nxwarp-quality -V         ... with pytest's output
#
# Configure with -DNXWARP_QUALITY_PYTHON=/path/to/python to use a specific
# interpreter (for example the venv described in tools/quality/README.md).
# Tests are skipped, not failed, when the interpreter lacks pytest or numpy:
# a machine without them should not break the codec build.

option(NXWARP_QUALITY_TESTS "Register the Python quality-harness tests with ctest" ON)

if(NOT NXWARP_QUALITY_TESTS)
  return()
endif()

# The root CMakeLists calls enable_testing() when NXWARP_BUILD_TESTS is ON,
# which is what generates the top-level CTest configuration. Calling it again
# here is harmless and keeps this directory self-contained if it is ever
# configured on its own. Note it does NOT rescue -DNXWARP_BUILD_TESTS=OFF:
# without the top-level call there is no test configuration for ctest to read,
# which is the right behaviour for someone who asked for no tests.
enable_testing()

set(NXWARP_QUALITY_PYTHON "" CACHE FILEPATH
    "Python interpreter used for the quality harness (default: found on PATH)")

if(NXWARP_QUALITY_PYTHON)
  set(_nxq_python "${NXWARP_QUALITY_PYTHON}")
else()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(_nxq_python "${Python3_EXECUTABLE}")
  endif()
endif()

if(NOT _nxq_python)
  message(STATUS "tools/quality: no Python 3 interpreter found, harness tests not registered")
  return()
endif()

set(_nxq_dir "${CMAKE_CURRENT_SOURCE_DIR}/quality")

# Probe once at configure time so a machine without pytest gets a clear status
# line rather than a red test.
execute_process(
  COMMAND "${_nxq_python}" -c "import pytest, numpy"
  RESULT_VARIABLE _nxq_deps
  OUTPUT_QUIET ERROR_QUIET
)

if(NOT _nxq_deps EQUAL 0)
  message(STATUS
    "tools/quality: ${_nxq_python} lacks pytest or numpy, harness tests not registered.\n"
    "   Create the venv described in tools/quality/README.md and reconfigure with\n"
    "   -DNXWARP_QUALITY_PYTHON=<venv>/bin/python")
  return()
endif()

add_test(
  NAME nxwarp-quality
  COMMAND "${_nxq_python}" -m pytest "${_nxq_dir}/tests" -q
  WORKING_DIRECTORY "${_nxq_dir}"
)

# The suite shells out to ffmpeg in one end-to-end test; keep it on the
# harness's core slice so it never competes with the interactive session.
set_tests_properties(nxwarp-quality PROPERTIES
  TIMEOUT 900
  LABELS "quality;python"
  ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

message(STATUS "tools/quality: registered ctest target 'nxwarp-quality' using ${_nxq_python}")
