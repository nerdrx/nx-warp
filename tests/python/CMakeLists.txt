# tests/python -- the nxvc Python bindings' pytest suite, registered with ctest
# as `python.pytest`.
#
#   ctest -R python.pytest        run it
#   ctest -R python.pytest -V     ... with pytest's output
#
# The bindings are pure Python over ctypes, so there is nothing to compile
# here; this file only wires the suite into ctest.
#
# Configure with -DNXWARP_PYTHON=/path/to/python to use a specific interpreter
# (for example the venv in python/README.md).  The test SKIPS (ctest return
# code 77) rather than failing when that interpreter has no pytest or numpy,
# and the end-to-end encode/decode tests inside it skip themselves, with the
# build command in the reason, when no nxvc shared library is found.
#
# The suite also parses every bitstream in tests/vectors/ with the pure-Python
# parser and, when an nxv-info binary is present, compares the two readings
# field for field.  That part needs no library and no vectors regeneration.

option(NXWARP_PYTHON_TESTS "Register the nxvc Python binding tests with ctest" ON)

if(NOT NXWARP_PYTHON_TESTS)
  return()
endif()

set(NXWARP_PYTHON "" CACHE FILEPATH
    "Python interpreter used for the nxvc bindings (default: found on PATH)")

if(NXWARP_PYTHON)
  set(_nxvc_python "${NXWARP_PYTHON}")
elseif(NXWARP_QUALITY_PYTHON)
  # tools/quality already asked for an interpreter; reuse it so one venv
  # serves both Python suites.
  set(_nxvc_python "${NXWARP_QUALITY_PYTHON}")
else()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(_nxvc_python "${Python3_EXECUTABLE}")
  endif()
endif()

if(NOT _nxvc_python)
  message(STATUS "tests/python: no Python 3 interpreter found, python.pytest not registered")
  return()
endif()

set(_nxvc_py_dir "${CMAKE_SOURCE_DIR}/python")

# ref/ builds nxvc_ref as a STATIC library by default, which ctypes cannot
# load; nxvc_ref_shared produces libnxvc_ref.so beside it.  Hand that target's
# path to the suite when it is configured; otherwise the loader still searches
# this build tree via NXVC_BUILD_DIR and the codec tests skip cleanly when
# nothing turns up.
set(_nxvc_library "")
if(TARGET nxvc_ref_shared)
  set(_nxvc_library "$<TARGET_FILE:nxvc_ref_shared>")
elseif(TARGET nxvc_shared)
  set(_nxvc_library "$<TARGET_FILE:nxvc_shared>")
endif()

add_test(
  NAME python.pytest
  COMMAND ${CMAKE_COMMAND}
          -DNXVC_PYTHON=${_nxvc_python}
          -DNXVC_PY_DIR=${_nxvc_py_dir}
          -DNXVC_LIBRARY=${_nxvc_library}
          -DNXVC_BUILD_DIR=${CMAKE_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/run_pytest.cmake
  WORKING_DIRECTORY "${_nxvc_py_dir}"
)

set_tests_properties(python.pytest PROPERTIES
  SKIP_RETURN_CODE 77
  TIMEOUT 600
  LABELS "python;bindings"
)

if(_nxvc_library)
  message(STATUS "tests/python: registered python.pytest using ${_nxvc_python} "
                 "(shared library target found)")
else()
  message(STATUS "tests/python: registered python.pytest using ${_nxvc_python} "
                 "(no shared nxvc target; the end-to-end tests will skip "
                 "unless NXVC_LIBRARY is set)")
endif()
