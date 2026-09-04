# Runs the nxvc Python binding tests, exiting 77 (ctest's SKIP_RETURN_CODE)
# when the interpreter cannot run them at all.
#
# The skip is decided at *run* time, not configure time, so a developer who
# pip-installs pytest into the configured interpreter does not have to
# reconfigure the build to get the test.
#
#   cmake -DNXVC_PYTHON=<python> -DNXVC_PY_DIR=<repo>/python
#         [-DNXVC_LIBRARY=<libnxvc_ref.so>] [-DNXVC_BUILD_DIR=<build>]
#         -P run_pytest.cmake

if(NOT NXVC_PYTHON OR NOT NXVC_PY_DIR)
  message(FATAL_ERROR "run_pytest.cmake needs -DNXVC_PYTHON and -DNXVC_PY_DIR")
endif()

# pytest and numpy are the hard requirements.  Missing them is a skip, not a
# failure: a machine without them should not turn the codec build red.
execute_process(
  COMMAND "${NXVC_PYTHON}" -c "import pytest, numpy"
  RESULT_VARIABLE _deps
  OUTPUT_QUIET ERROR_QUIET
)
if(NOT _deps EQUAL 0)
  message(STATUS
    "python.pytest: ${NXVC_PYTHON} lacks pytest or numpy -- skipping.\n"
    "  python3 -m venv --system-site-packages <venv> && "
    "<venv>/bin/pip install pytest -e ${NXVC_PY_DIR}\n"
    "  then reconfigure with -DNXWARP_PYTHON=<venv>/bin/python")
  message(STATUS "SKIP")
  cmake_language(EXIT 77)
endif()

# The package itself must be importable.  It is pure Python over ctypes, so
# `pip install -e python/` is the only build step there is; when it is not
# installed, fall back to putting src/ on PYTHONPATH so the suite still runs
# out of a plain checkout.
execute_process(
  COMMAND "${NXVC_PYTHON}" -c "import nxvc"
  RESULT_VARIABLE _pkg
  OUTPUT_QUIET ERROR_QUIET
)
set(_env)
if(NOT _pkg EQUAL 0)
  if(DEFINED ENV{PYTHONPATH} AND NOT "$ENV{PYTHONPATH}" STREQUAL "")
    set(ENV{PYTHONPATH} "${NXVC_PY_DIR}/src:$ENV{PYTHONPATH}")
  else()
    set(ENV{PYTHONPATH} "${NXVC_PY_DIR}/src")
  endif()
  execute_process(
    COMMAND "${NXVC_PYTHON}" -c "import nxvc"
    RESULT_VARIABLE _pkg2
    OUTPUT_QUIET ERROR_QUIET
  )
  if(NOT _pkg2 EQUAL 0)
    message(STATUS "python.pytest: cannot import nxvc from ${NXVC_PY_DIR}/src -- skipping")
    message(STATUS "SKIP")
    cmake_language(EXIT 77)
  endif()
endif()

# Point the loader at this build tree.  The end-to-end tests skip themselves
# with a clear reason when no shared library turns up (ref/ currently builds
# nxvc_ref as a static library only), so this is best-effort, not required.
if(NXVC_LIBRARY AND EXISTS "${NXVC_LIBRARY}")
  set(ENV{NXVC_LIBRARY} "${NXVC_LIBRARY}")
endif()
if(NXVC_BUILD_DIR)
  set(ENV{NXVC_BUILD_DIR} "${NXVC_BUILD_DIR}")
endif()
set(ENV{PYTHONDONTWRITEBYTECODE} "1")

execute_process(
  COMMAND "${NXVC_PYTHON}" -m pytest "${NXVC_PY_DIR}/tests" -q
  WORKING_DIRECTORY "${NXVC_PY_DIR}"
  RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "pytest failed with ${_rc}")
endif()
