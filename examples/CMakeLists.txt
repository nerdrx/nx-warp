# examples/ -- small, readable programs against the public APIs.
#
# Nothing here is part of the library, nothing here is installed, and nothing
# here is a test.  These are the programs you read to learn the API and run to
# see what a stream actually contains.
#
# Every target is guarded on the libraries it needs *existing as targets*, not
# on a find_package: components land at different times and a missing sibling
# must never break the configure of the ones that are here.  Configuring this
# directory with no components at all is legal and prints one status line.

# The C ABI header is written alongside the reference codec.  Guard on the file
# so a checkout mid-landing configures cleanly instead of erroring in the
# compiler.
set(_nxvc_header "${CMAKE_SOURCE_DIR}/include/nxvc/nxvc.h")
if(EXISTS "${_nxvc_header}" AND TARGET nxvc_ref)
  set(_nxvc_ok TRUE)
else()
  set(_nxvc_ok FALSE)
endif()

set(_nxvc_examples_built "")
set(_nxvc_examples_skipped "")

# ---------------------------------------------------------------- codec ABI
if(_nxvc_ok)
  # encode_frame and decode_frame are C, on purpose: the ABI is a C ABI, and if
  # they ever need a C++ compiler the ABI has grown a C++ dependency it should
  # not have.  LINKER_LANGUAGE CXX is still required because the *library*
  # behind that ABI is C++ and pulls in libstdc++.
  foreach(ex encode_frame decode_frame)
    string(REPLACE "_frame" "" _short ${ex})
    add_executable(nxvc-example-${_short} ${ex}.c)
    target_link_libraries(nxvc-example-${_short} PRIVATE nxvc_ref)
    set_target_properties(nxvc-example-${_short} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
    list(APPEND _nxvc_examples_built nxvc-example-${_short})
  endforeach()

  add_executable(nxvc-example-roundtrip roundtrip_psnr.cpp)
  add_executable(nxvc-example-tilewalk tile_walk.cpp)
  foreach(t nxvc-example-roundtrip nxvc-example-tilewalk)
    target_link_libraries(${t} PRIVATE nxvc_ref)
    set_target_properties(${t} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    list(APPEND _nxvc_examples_built ${t})
  endforeach()
else()
  list(APPEND _nxvc_examples_skipped
       "encode/decode/roundtrip/tilewalk (need target nxvc_ref and include/nxvc/nxvc.h)")
endif()

# ------------------------------------------------------------------ transport
if(TARGET nxvc_transport)
  add_executable(nxvc-example-loopback transport_loopback.cpp)
  target_link_libraries(nxvc-example-loopback PRIVATE nxvc_transport)
  set_target_properties(nxvc-example-loopback PROPERTIES
      CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  # The loopback runs on synthetic tile sizes alone; with the codec present it
  # can also read real tile lengths out of a .nxv.  Optional, hence a define
  # rather than a hard dependency.
  if(_nxvc_ok)
    target_link_libraries(nxvc-example-loopback PRIVATE nxvc_ref)
    target_compile_definitions(nxvc-example-loopback PRIVATE NXVC_EXAMPLE_HAVE_REF)
  endif()
  list(APPEND _nxvc_examples_built nxvc-example-loopback)
else()
  list(APPEND _nxvc_examples_skipped "loopback (needs target nxvc_transport)")
endif()

# ------------------------------------------------------------------- warnings
foreach(t IN LISTS _nxvc_examples_built)
  if(NOT MSVC)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  # Examples are documentation; keep them out of the install set and out of the
  # default `all` of a library-only build only if the user asked for that.
  set_target_properties(${t} PROPERTIES FOLDER examples)
endforeach()

if(_nxvc_examples_built)
  list(JOIN _nxvc_examples_built " " _b)
  message(STATUS "examples: building ${_b}")
endif()
foreach(s IN LISTS _nxvc_examples_skipped)
  message(STATUS "examples: skipping ${s}")
endforeach()

# ---------------------------------------------------------------- smoke tests
# The examples double as the cheapest possible end-to-end check: generate a
# tiny picture, encode it, decode it, walk it.  Registered only when tests are
# on, and named `examples.<name>` to match the repo's `<component>.<name>`
# convention (see TESTING.md).
if(NXWARP_BUILD_TESTS AND _nxvc_ok)
  add_test(NAME examples.smoke
           COMMAND ${CMAKE_COMMAND}
                   -DENC=$<TARGET_FILE:nxvc-example-encode>
                   -DDEC=$<TARGET_FILE:nxvc-example-decode>
                   -DRT=$<TARGET_FILE:nxvc-example-roundtrip>
                   -DTW=$<TARGET_FILE:nxvc-example-tilewalk>
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/smoke
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/smoke.cmake)
  set_tests_properties(examples.smoke PROPERTIES LABELS "examples" TIMEOUT 120)
endif()

if(NXWARP_BUILD_TESTS AND TARGET nxvc-example-loopback)
  # Lossless link: every tile must arrive and the shadow must agree, so the
  # example's own exit code is the assertion.
  add_test(NAME examples.loopback_clean
           COMMAND nxvc-example-loopback --cols 20 --rows 12 --loss 0.0)
  # Lossy link: still must not diverge -- concealment is expected, disagreement
  # about it is not.
  add_test(NAME examples.loopback_lossy
           COMMAND nxvc-example-loopback --cols 20 --rows 12 --loss 0.08 --burst 3)
  set_tests_properties(examples.loopback_clean examples.loopback_lossy
                       PROPERTIES LABELS "examples" TIMEOUT 120)
endif()
