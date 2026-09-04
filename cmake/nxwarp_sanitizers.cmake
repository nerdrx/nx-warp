# nxwarp_sanitizers.cmake -- sanitizer and coverage instrumentation.
#
# NXWARP_SANITIZER is a single string so the presets stay readable and the
# mutually-exclusive combinations are rejected here rather than at link time:
#
#   off              no instrumentation                         (default)
#   address          AddressSanitizer
#   undefined        UndefinedBehaviorSanitizer
#   address+undefined  both, the CI configuration
#   thread           ThreadSanitizer (incompatible with address)
#   memory           MemorySanitizer, clang only, needs an instrumented libc++
#
# Sanitizers are applied globally (CMAKE_<LANG>_FLAGS + the link flags) rather
# than through an INTERFACE target: an ASan build in which only some objects
# are instrumented reports false positives, so partial adoption is worse than
# none.  This is why a sanitizer build is its own build directory.
#
# Related, and separate:
#   NXWARP_COVERAGE   gcov/llvm-cov instrumentation
#   NXVC_FUZZ         libFuzzer entry points (the option the nightly grep looks
#                     for lives in ref/; this file only adds the flags)

include_guard(GLOBAL)

set(NXWARP_SANITIZER "off" CACHE STRING "Sanitizer: off|address|undefined|address+undefined|thread|memory")
set_property(CACHE NXWARP_SANITIZER PROPERTY STRINGS
             off address undefined address+undefined thread memory)

option(NXWARP_COVERAGE "Instrument for gcov / llvm-cov coverage" OFF)
option(NXWARP_FUZZ "Build libFuzzer entry points (-fsanitize=fuzzer-no-link)" OFF)

set(_nxwarp_san_c_flags "")
set(_nxwarp_san_link_flags "")

if(NOT NXWARP_SANITIZER STREQUAL "off")
    if(MSVC AND NOT NXWARP_SANITIZER STREQUAL "address")
        message(FATAL_ERROR "MSVC supports only NXWARP_SANITIZER=address")
    endif()

    set(_nxwarp_san_list "")
    if(NXWARP_SANITIZER MATCHES "address")
        list(APPEND _nxwarp_san_list address)
    endif()
    if(NXWARP_SANITIZER MATCHES "undefined")
        list(APPEND _nxwarp_san_list undefined)
    endif()
    if(NXWARP_SANITIZER MATCHES "thread")
        list(APPEND _nxwarp_san_list thread)
    endif()
    if(NXWARP_SANITIZER MATCHES "memory")
        list(APPEND _nxwarp_san_list memory)
    endif()
    if(NOT _nxwarp_san_list)
        message(FATAL_ERROR "unknown NXWARP_SANITIZER value '${NXWARP_SANITIZER}'")
    endif()

    if("thread" IN_LIST _nxwarp_san_list AND "address" IN_LIST _nxwarp_san_list)
        message(FATAL_ERROR "ThreadSanitizer and AddressSanitizer cannot be combined")
    endif()
    if("memory" IN_LIST _nxwarp_san_list AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MemorySanitizer requires clang")
    endif()

    list(JOIN _nxwarp_san_list "," _nxwarp_san_joined)
    list(APPEND _nxwarp_san_c_flags "-fsanitize=${_nxwarp_san_joined}"
                                    -fno-omit-frame-pointer
                                    -fno-optimize-sibling-calls
                                    -g)
    list(APPEND _nxwarp_san_link_flags "-fsanitize=${_nxwarp_san_joined}")

    if("undefined" IN_LIST _nxwarp_san_list)
        # A UBSan report that does not abort is a report nobody notices in CI.
        list(APPEND _nxwarp_san_c_flags
             -fno-sanitize-recover=all
             -fsanitize=float-divide-by-zero)
        # The reference codec is integer-only by rule, so the integer overflow
        # checks are exactly the ones worth having.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            list(APPEND _nxwarp_san_c_flags -fsanitize=integer
                                            -fno-sanitize=unsigned-integer-overflow)
        endif()
    endif()

    message(STATUS "sanitizers: ${_nxwarp_san_joined}")
endif()

# ---------------------------------------------------------------------------
# Coverage
# ---------------------------------------------------------------------------
if(NXWARP_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _nxwarp_san_c_flags -fprofile-instr-generate -fcoverage-mapping)
        list(APPEND _nxwarp_san_link_flags -fprofile-instr-generate -fcoverage-mapping)
        message(STATUS "coverage: llvm-cov (-fprofile-instr-generate -fcoverage-mapping)")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND _nxwarp_san_c_flags --coverage -fprofile-abs-path)
        list(APPEND _nxwarp_san_link_flags --coverage)
        message(STATUS "coverage: gcov (--coverage)")
    else()
        message(WARNING "NXWARP_COVERAGE is not supported for ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# ---------------------------------------------------------------------------
# libFuzzer
#
# -fsanitize=fuzzer-no-link everywhere, so any translation unit may define
# LLVMFuzzerTestOneInput; the target that wants to be a fuzzer links
# -fsanitize=fuzzer itself.  NXVC_FUZZ is defined as a compile definition
# because that is the spelling the nightly workflow and the existing component
# code look for.
# ---------------------------------------------------------------------------
if(NXWARP_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "NXWARP_FUZZ needs clang (libFuzzer)")
    endif()
    list(APPEND _nxwarp_san_c_flags -fsanitize=fuzzer-no-link)
    add_compile_definitions(NXVC_FUZZ=1 NXWARP_FUZZ=1)
    message(STATUS "fuzzing: libFuzzer entry points enabled (NXVC_FUZZ=1)")
endif()

# ---------------------------------------------------------------------------
# Apply.
# ---------------------------------------------------------------------------
if(_nxwarp_san_c_flags)
    add_compile_options(${_nxwarp_san_c_flags})
endif()
if(_nxwarp_san_link_flags)
    add_link_options(${_nxwarp_san_link_flags})
endif()

function(nxwarp_sanitizers_summary)
    message(STATUS "  sanitizer         : ${NXWARP_SANITIZER}")
    message(STATUS "  coverage          : ${NXWARP_COVERAGE}")
    message(STATUS "  fuzzing           : ${NXWARP_FUZZ}")
endfunction()
