# nxvc_ref reference-codec tests.  Registered as ref.<name> so that
# `ctest -R ref.` selects exactly this suite.
set(NXVC_REF_TEST_INC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ref/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

foreach(t transform rans codec headers fuzz_smoke inter)
    add_executable(ref_test_${t} test_${t}.cpp)
    target_link_libraries(ref_test_${t} PRIVATE nxvc_ref)
    target_include_directories(ref_test_${t} PRIVATE ${NXVC_REF_TEST_INC})
    set_target_properties(ref_test_${t} PROPERTIES CXX_STANDARD 20
                          CXX_STANDARD_REQUIRED ON)
    add_test(NAME ref.${t} COMMAND ref_test_${t})
endforeach()

# Development tool: regenerates ref/src/default_tables.inc.  Not a test.
add_executable(nxv-gentables gentables.cpp)
target_link_libraries(nxv-gentables PRIVATE nxvc_ref)
target_include_directories(nxv-gentables PRIVATE ${NXVC_REF_TEST_INC})
set_target_properties(nxv-gentables PROPERTIES CXX_STANDARD 20
                      CXX_STANDARD_REQUIRED ON)

# Conformance vectors: one binary generates and checks them.
add_executable(nxv-vectors vectors.cpp)
target_link_libraries(nxv-vectors PRIVATE nxvc_ref)
target_include_directories(nxv-vectors PRIVATE ${NXVC_REF_TEST_INC})
set_target_properties(nxv-vectors PROPERTIES CXX_STANDARD 20
                      CXX_STANDARD_REQUIRED ON)
add_test(NAME ref.vectors
         COMMAND nxv-vectors --check ${CMAKE_SOURCE_DIR}/tests/vectors)

# Range safety of the normative decode path.  Its point is to be run under the
# asan-ubsan preset, where a signed overflow in the inverse transform aborts:
#   cmake --preset asan-ubsan && cmake --build --preset asan-ubsan
#   ctest --preset asan-ubsan -R 'ref\.(saturate|vectors)'
add_executable(ref_test_saturate test_saturate.cpp)
target_link_libraries(ref_test_saturate PRIVATE nxvc_ref)
target_include_directories(ref_test_saturate PRIVATE ${NXVC_REF_TEST_INC})
set_target_properties(ref_test_saturate PROPERTIES CXX_STANDARD 20
                      CXX_STANDARD_REQUIRED ON)
add_test(NAME ref.saturate
         COMMAND ref_test_saturate ${CMAKE_SOURCE_DIR}/tests/vectors)

# End-to-end CLI test: nxv-enc | nxv-dec | nxv-info must agree.
if(TARGET nxv-enc)
    add_executable(ref_test_cli test_cli.cpp)
    target_link_libraries(ref_test_cli PRIVATE nxvc_ref)
    target_include_directories(ref_test_cli PRIVATE ${NXVC_REF_TEST_INC})
    set_target_properties(ref_test_cli PROPERTIES CXX_STANDARD 20
                          CXX_STANDARD_REQUIRED ON)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cli)
    add_test(NAME ref.cli COMMAND ref_test_cli
             $<TARGET_FILE:nxv-enc> $<TARGET_FILE:nxv-dec> $<TARGET_FILE:nxv-info>
             ${CMAKE_CURRENT_BINARY_DIR}/cli)
endif()

# libFuzzer target (nightly CI).  Requires a clang toolchain.
option(NXVC_FUZZ "Build the nxvc_ref libFuzzer targets" OFF)
if(NXVC_FUZZ)
    # The library sources are compiled into the target so the fuzzer sees
    # instrumented codec code, not a plain static archive.
    add_executable(nxvc_fuzz_decode fuzz_decode.cpp
        ${CMAKE_SOURCE_DIR}/ref/src/tables.cpp
        ${CMAKE_SOURCE_DIR}/ref/src/transform.cpp
        ${CMAKE_SOURCE_DIR}/ref/src/entropy.cpp
        ${CMAKE_SOURCE_DIR}/ref/src/codec.cpp)
    target_include_directories(nxvc_fuzz_decode PRIVATE ${NXVC_REF_TEST_INC})
    target_compile_options(nxvc_fuzz_decode PRIVATE
                           -fsanitize=fuzzer,address,undefined)
    target_link_options(nxvc_fuzz_decode PRIVATE
                        -fsanitize=fuzzer,address,undefined)
    set_target_properties(nxvc_fuzz_decode PROPERTIES CXX_STANDARD 20)
endif()
