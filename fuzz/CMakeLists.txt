# fuzz/ -- libFuzzer harnesses and their always-built regression runners.
#
# Two binaries come out of every harness source:
#
#   <target>          a real libFuzzer binary.  Needs clang and NXVC_FUZZ=ON.
#   <target>_replay   a plain executable that replays a corpus directory and
#                     drives the custom mutator.  Built with any compiler, in
#                     every configuration, and registered with CTest, so the
#                     checked-in reproducers under fuzz/regressions/ run on
#                     every push even where clang is not installed.
#
# Every target is guarded on the component it exercises actually being present
# (the components land in parallel), so this directory never breaks configure.
#
# Recommended fuzzing configuration -- NXWARP_FUZZ is what instruments the
# component libraries themselves, without which libFuzzer gets no coverage
# feedback from the code under test and degenerates into random testing:
#
#   cmake -S . -B build-fuzz -G Ninja \
#     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     -DNXVC_FUZZ=ON -DNXWARP_FUZZ=ON \
#     -DNXWARP_SANITIZER=address+undefined \
#     -DNXWARP_BUILD_VK=OFF -DNXWARP_BUILD_TOOLS=OFF
#
# See fuzz/README.md.

# NXVC_FUZZ is also declared by tests/ref/CMakeLists.txt; option() is
# idempotent and both use the same default, so -DNXVC_FUZZ=ON turns on both
# suites at once, which is what the nightly workflow wants.
option(NXVC_FUZZ "Build the libFuzzer targets (needs clang)" OFF)

set(NXFUZZ_COMMON_SRC ${CMAKE_CURRENT_SOURCE_DIR}/common/nxfuzz_main.cpp)
set(NXFUZZ_INC ${CMAKE_CURRENT_SOURCE_DIR})

set(NXFUZZ_ALL_TARGETS "")

# ---------------------------------------------------------------------------
# nxfuzz_add_target(<name> LIB <component target> [INCLUDE <dirs>...])
# ---------------------------------------------------------------------------
function(nxfuzz_add_target name)
    cmake_parse_arguments(A "" "LIB" "INCLUDE" ${ARGN})
    if(NOT TARGET ${A_LIB})
        message(STATUS "fuzz: ${name} skipped, target ${A_LIB} does not exist yet")
        return()
    endif()

    # ---- the regression runner: always built, no libFuzzer, any compiler ----
    add_executable(${name}_replay ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp ${NXFUZZ_COMMON_SRC})
    target_link_libraries(${name}_replay PRIVATE ${A_LIB})
    target_include_directories(${name}_replay PRIVATE ${NXFUZZ_INC} ${A_INCLUDE})
    target_compile_definitions(${name}_replay PRIVATE NXFUZZ_STANDALONE=1)
    set_target_properties(${name}_replay PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    if(NOT MSVC)
        # The harnesses deliberately compare, cast and truncate; -Wall is
        # useful, -Werror here would make an unrelated component warning fail
        # the fuzz build.
        target_compile_options(${name}_replay PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()

    # A replay of the checked-in reproducers plus a mutator workout.  The
    # mutator rounds are the part that fails when a structure-aware mutator
    # writes past max_size, which is the classic harness bug.
    if(NXWARP_BUILD_TESTS)
        add_test(NAME fuzz.${name}.replay
                 COMMAND ${name}_replay --quiet --timeout 30 --mutate 400
                         ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${name}
                         ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name})
        set_tests_properties(fuzz.${name}.replay PROPERTIES TIMEOUT 300)
    endif()

    # ---- the libFuzzer binary ----------------------------------------------
    if(NXVC_FUZZ)
        add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
        target_link_libraries(${name} PRIVATE ${A_LIB})
        target_include_directories(${name} PRIVATE ${NXFUZZ_INC} ${A_INCLUDE})
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer)
        set_target_properties(${name} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
        if(NOT MSVC)
            target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
        endif()
    endif()

    set(NXFUZZ_ALL_TARGETS ${NXFUZZ_ALL_TARGETS} ${name} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
if(NXWARP_BUILD_TESTS)
    # fuzz/ is added from the component loop, which runs before the top-level
    # enable_testing().  Enabling it here is what makes the fuzz.* tests appear
    # in this directory's CTestTestfile.
    enable_testing()
endif()

# ref/ -- the reference codec.  The rANS target reaches into ref/src, which is
# the library's private header directory; it is testing the entropy coder, not
# the public C ABI.
nxfuzz_add_target(nxvc_decode_fuzz  LIB nxvc_ref
                  INCLUDE ${CMAKE_SOURCE_DIR}/include)
nxfuzz_add_target(nxvc_headers_fuzz LIB nxvc_ref
                  INCLUDE ${CMAKE_SOURCE_DIR}/include)
nxfuzz_add_target(nxvc_rans_fuzz    LIB nxvc_ref
                  INCLUDE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/ref/src)

# transport/
nxfuzz_add_target(transport_depacketize_fuzz LIB nxvc_transport)
nxfuzz_add_target(transport_rs_fuzz          LIB nxvc_transport)
nxfuzz_add_target(transport_feedback_fuzz    LIB nxvc_transport)

# warp/
nxfuzz_add_target(warp_tile_fuzz LIB nxvc_warp_ref)

# ---------------------------------------------------------------------------
# Seed corpora
# ---------------------------------------------------------------------------
# `cmake --build . --target nxfuzz-corpus` regenerates fuzz/corpus/ from
# tests/vectors/ plus a synthetic generator.  The result is checked in, so a
# fresh clone (and OSS-Fuzz) has seeds without running anything.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(nxfuzz-corpus
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_corpus.py
                --repo ${CMAKE_SOURCE_DIR} --out ${CMAKE_CURRENT_SOURCE_DIR}/corpus
        COMMENT "fuzz: regenerating the seed corpora"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Diagnostics
# ---------------------------------------------------------------------------
if(NXVC_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "NXVC_FUZZ requires clang (libFuzzer); got ${CMAKE_CXX_COMPILER_ID}")
    endif()
    if(NOT NXWARP_FUZZ AND NOT CMAKE_CXX_FLAGS MATCHES "fuzzer-no-link")
        message(WARNING
            "fuzz: NXVC_FUZZ=ON without NXWARP_FUZZ=ON. The component libraries "
            "will not carry libFuzzer coverage instrumentation, so the fuzzer "
            "gets no feedback from the code under test. Add -DNXWARP_FUZZ=ON.")
    endif()
    message(STATUS "fuzz: libFuzzer targets enabled")
endif()
