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
# What 'undefined' actually turns on is NXWARP_UBSAN_CHECKS, below -- not
# clang's `integer` group, which is not about undefined behaviour at all.
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

# ---------------------------------------------------------------------------
# Which UBSan checks 'undefined' means.
#
# Deliberately NOT clang's `integer` group. That group bundles
# implicit-integer-truncation, implicit-integer-sign-change and
# unsigned-shift-base, none of which are undefined behaviour: they fire on
# correct, intentional unsigned wraparound and narrowing. The rANS coder does
# it, hashing does it, and libstdc++ does it in headers we do not control. An
# earlier version of this file turned the group on and 26 of 45 tests failed
# on code that was not wrong -- which trains people to ignore the sanitizer,
# the opposite of the point.
#
# The `undefined` group already carries the checks that matter for an
# integer-only reference codec: signed-integer-overflow, shift, array bounds,
# null, alignment, object-size, vla-bound, return, float-cast-overflow.
#
# This is a cache variable because these flags go through add_compile_options(),
# which lands *after* CMAKE_CXX_FLAGS -- so a -fno-sanitize=... on the command
# line cannot undo them. Change the policy here instead:
#
#   -DNXWARP_UBSAN_CHECKS=undefined,integer        everything, noise included
#   -DNXWARP_UBSAN_CHECKS=signed-integer-overflow  just the one you are chasing
set(NXWARP_UBSAN_CHECKS "undefined,float-divide-by-zero" CACHE STRING
    "Comma-separated -fsanitize= list meant by NXWARP_SANITIZER=...undefined")

# A UBSan report that does not abort is a report nobody notices in CI. Turn
# this ON to collect every report in one run instead of stopping at the first.
option(NXWARP_SANITIZE_RECOVER "Let sanitizer reports continue instead of aborting" OFF)

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

    # Expand the bare 'undefined' token into the configured check list.
    set(_nxwarp_san_expanded "")
    foreach(_s IN LISTS _nxwarp_san_list)
        if(_s STREQUAL "undefined")
            list(APPEND _nxwarp_san_expanded "${NXWARP_UBSAN_CHECKS}")
        else()
            list(APPEND _nxwarp_san_expanded "${_s}")
        endif()
    endforeach()
    list(JOIN _nxwarp_san_expanded "," _nxwarp_san_joined)
    list(APPEND _nxwarp_san_c_flags "-fsanitize=${_nxwarp_san_joined}"
                                    -fno-omit-frame-pointer
                                    -fno-optimize-sibling-calls
                                    -g)
    list(APPEND _nxwarp_san_link_flags "-fsanitize=${_nxwarp_san_joined}")

    if("undefined" IN_LIST _nxwarp_san_list AND NOT NXWARP_SANITIZE_RECOVER)
        list(APPEND _nxwarp_san_c_flags -fno-sanitize-recover=all)
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
    if(NXWARP_SANITIZER MATCHES "undefined")
        message(STATUS "  ubsan checks      : ${NXWARP_UBSAN_CHECKS}")
        message(STATUS "  ubsan recovers    : ${NXWARP_SANITIZE_RECOVER}")
    endif()
    message(STATUS "  coverage          : ${NXWARP_COVERAGE}")
    message(STATUS "  fuzzing           : ${NXWARP_FUZZ}")
endfunction()
