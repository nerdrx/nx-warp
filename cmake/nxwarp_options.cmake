# nxwarp_options.cmake -- project-wide build hygiene.
#
# Nothing here reaches into a component's CMakeLists.  The warning set is
# carried by an INTERFACE target, nxwarp::warnings, which components opt into:
#
#     target_link_libraries(nxvc_ref PRIVATE nxwarp::warnings)
#
# so third-party sources, generated code and vendored headers keep their own
# (lack of) warning policy.  IPO and ccache are global because they are
# properties of how the tree is built, not of what a component is.
#
# Options:
#   NXWARP_WERROR            warnings are errors                    (default OFF)
#   NXWARP_WARN_CONVERSION   add -Wconversion / -Wsign-conversion   (default ON)
#   NXWARP_IPO               link-time optimisation, Release only    (default OFF)
#   NXWARP_CCACHE            use ccache/sccache when present         (default ON)
#   NXWARP_EXPORT_COMPILE_COMMANDS  write compile_commands.json      (default ON)

include_guard(GLOBAL)
include(CheckIPOSupported)

option(NXWARP_WERROR "Treat compiler warnings as errors" OFF)
option(NXWARP_WARN_CONVERSION "Enable -Wconversion / -Wsign-conversion" ON)
option(NXWARP_IPO "Enable interprocedural optimisation (LTO)" OFF)
option(NXWARP_CCACHE "Use ccache/sccache as the compiler launcher when found" ON)
option(NXWARP_EXPORT_COMPILE_COMMANDS "Write compile_commands.json" ON)

if(NXWARP_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
endif()

# A default build type, so a bare `cmake -S . -B build` is not an unoptimised
# no-debug-info mystery.  Multi-config generators pick their own.
get_property(_nxwarp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _nxwarp_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "CMAKE_BUILD_TYPE was empty, defaulting to RelWithDebInfo")
endif()

# Compiler extensions are deliberately left at CMake's default (ON, i.e.
# -std=gnu++20). Forcing -std=c++20 sounds tidier and is a portability trap:
# glibc and the mingw headers gate M_PI and friends on __STRICT_ANSI__, so
# strict mode removes them, and several components use M_PI. Turning this off
# is a tree-wide source change, not a build-system flag; if we want it, it is
# its own commit that adds <numbers> to the callers first.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Static libraries and CLIs land in one place per build tree; the quality
# harness and the scripts look for them there rather than crawling the tree.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# nxwarp::warnings
# ---------------------------------------------------------------------------
add_library(nxwarp_warnings INTERFACE)
add_library(nxwarp::warnings ALIAS nxwarp_warnings)

set(_nxwarp_gnuish
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wcast-qual
    -Wcast-align
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wundef
    -Wunused
    # The reference codec uses short single-letter locals in the DCT flow
    # graphs on purpose; unused *parameters* are common in the not-yet-wired
    # component seams.  Neither is a defect worth a wall of noise.
    -Wno-unused-parameter)

# C++ only -- GCC and Clang both complain if these reach the C driver.
set(_nxwarp_gnuish_cxx -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)

if(NXWARP_WARN_CONVERSION)
    list(APPEND _nxwarp_gnuish -Wconversion -Wsign-conversion)
endif()

set(_nxwarp_msvcish /W4 /permissive- /Zc:__cplusplus /utf-8)

target_compile_options(nxwarp_warnings INTERFACE
    "$<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:${_nxwarp_gnuish}>"
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:${_nxwarp_gnuish};${_nxwarp_gnuish_cxx}>"
    "$<$<COMPILE_LANG_AND_ID:C,MSVC>:${_nxwarp_msvcish}>"
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:${_nxwarp_msvcish}>")

if(NXWARP_WERROR)
    target_compile_options(nxwarp_warnings INTERFACE
        "$<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Werror>"
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Werror>"
        "$<$<COMPILE_LANG_AND_ID:C,MSVC>:/WX>"
        "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX>")
endif()

# ---------------------------------------------------------------------------
# nxwarp::deterministic -- flags the bit-exactness rule depends on.
#
# CONTRIBUTING.md: the reference decoder is the oracle and every other decoder
# must match it bit for bit.  -ffp-contract=off and no -ffast-math keep the
# host compiler from reassociating the float paths in the encoder-side
# analysis; the normative decode path is integer only and unaffected either
# way, but a component that does touch floats can link this and stop worrying.
# ---------------------------------------------------------------------------
add_library(nxwarp_deterministic INTERFACE)
add_library(nxwarp::deterministic ALIAS nxwarp_deterministic)
target_compile_options(nxwarp_deterministic INTERFACE
    "$<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-ffp-contract=off>"
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-ffp-contract=off>"
    "$<$<COMPILE_LANG_AND_ID:C,MSVC>:/fp:strict>"
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/fp:strict>")

# ---------------------------------------------------------------------------
# ccache
# ---------------------------------------------------------------------------
if(NXWARP_CCACHE AND NOT CMAKE_C_COMPILER_LAUNCHER AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(NXWARP_CCACHE_PROGRAM NAMES ccache sccache)
    if(NXWARP_CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${NXWARP_CCACHE_PROGRAM}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${NXWARP_CCACHE_PROGRAM}" CACHE STRING "" FORCE)
        message(STATUS "compiler launcher: ${NXWARP_CCACHE_PROGRAM}")
    endif()
endif()

# ---------------------------------------------------------------------------
# IPO / LTO
# ---------------------------------------------------------------------------
if(NXWARP_IPO)
    check_ipo_supported(RESULT _nxwarp_ipo_ok OUTPUT _nxwarp_ipo_why LANGUAGES C CXX)
    if(_nxwarp_ipo_ok)
        # Release configurations only: LTO on a Debug build costs link time and
        # buys nothing, and it makes sanitizer stack traces worse.
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
        message(STATUS "IPO/LTO: enabled for Release configurations")
    else()
        message(WARNING "IPO/LTO requested but unsupported: ${_nxwarp_ipo_why}")
    endif()
endif()

function(nxwarp_options_summary)
    message(STATUS "  warnings-as-errors: ${NXWARP_WERROR}")
    message(STATUS "  -Wconversion      : ${NXWARP_WARN_CONVERSION}")
    message(STATUS "  IPO/LTO           : ${NXWARP_IPO}")
    message(STATUS "  compiler launcher : ${CMAKE_CXX_COMPILER_LAUNCHER}")
endfunction()
