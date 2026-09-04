# nxwarp_version.cmake -- version discovery and the generated version header.
#
# Two independent version numbers live in this project (RELEASE.md explains the
# policy):
#
#   * the LIBRARY version, SemVer, derived from `git describe --tags --dirty`
#     when the tree is a git checkout with tags, and from project(VERSION ...)
#     otherwise (tarball export, shallow CI clone, fresh repo with no tags);
#   * the BITSTREAM version, a single integer, paper section 1.2, bumped only
#     when the wire format changes.  It is NOT tied to the library version.
#
# Both land in a generated header, `include/nxvc/version.h`, written into the
# build tree (never the source tree) at ${NXWARP_GENERATED_INCLUDE_DIR}.
#
# Exposed to the rest of the build:
#   NXVC_VERSION_MAJOR / _MINOR / _PATCH   integers
#   NXVC_VERSION                           "X.Y.Z"
#   NXVC_VERSION_FULL                      "X.Y.Z-<n>-g<sha>[-dirty]" or "X.Y.Z"
#   NXVC_VERSION_GIT                       raw git describe output, or ""
#   NXVC_VERSION_IS_DIRTY                  TRUE/FALSE
#   NXWARP_BITSTREAM_VERSION               cache variable, default 1
#   NXWARP_GENERATED_INCLUDE_DIR           build-tree include root

include_guard(GLOBAL)

set(NXWARP_BITSTREAM_VERSION 1 CACHE STRING
    "NX Warp bitstream format version (paper 1.2 stream header 'version' field)")

set(NXWARP_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include"
    CACHE INTERNAL "Build-tree include root for generated headers")

# ---------------------------------------------------------------------------
# Ask git.  Anything that goes wrong here is not an error: a source export has
# no .git, CI clones are frequently shallow and untagged, and a contributor's
# fresh checkout has no tags until the first release.
# ---------------------------------------------------------------------------
set(NXVC_VERSION_GIT "")
set(NXVC_VERSION_IS_DIRTY FALSE)
set(_nxvc_git_ok FALSE)

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _nxvc_describe
        ERROR_VARIABLE _nxvc_describe_err
        RESULT_VARIABLE _nxvc_describe_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_nxvc_describe_rc EQUAL 0 AND _nxvc_describe)
        set(NXVC_VERSION_GIT "${_nxvc_describe}")
        if(_nxvc_describe MATCHES "-dirty$")
            set(NXVC_VERSION_IS_DIRTY TRUE)
        endif()
        # v1.2.3 / 1.2.3 / v1.2.3-4-gabc1234[-dirty]
        if(_nxvc_describe MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
            set(NXVC_VERSION_MAJOR "${CMAKE_MATCH_1}")
            set(NXVC_VERSION_MINOR "${CMAKE_MATCH_2}")
            set(NXVC_VERSION_PATCH "${CMAKE_MATCH_3}")
            set(_nxvc_git_ok TRUE)
        endif()
    endif()

    # Re-run configure when HEAD moves, so the header does not go stale.
    foreach(_watch HEAD packed-refs)
        if(EXISTS "${CMAKE_SOURCE_DIR}/.git/${_watch}")
            set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND
                         PROPERTY CMAKE_CONFIGURE_DEPENDS
                         "${CMAKE_SOURCE_DIR}/.git/${_watch}")
        endif()
    endforeach()
endif()

if(NOT _nxvc_git_ok)
    set(NXVC_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
    set(NXVC_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
    set(NXVC_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
endif()

set(NXVC_VERSION "${NXVC_VERSION_MAJOR}.${NXVC_VERSION_MINOR}.${NXVC_VERSION_PATCH}")
if(NXVC_VERSION_GIT)
    set(NXVC_VERSION_FULL "${NXVC_VERSION_GIT}")
else()
    set(NXVC_VERSION_FULL "${NXVC_VERSION}")
endif()

if(_nxvc_git_ok)
    message(STATUS "nxvc version: ${NXVC_VERSION_FULL} (from git describe)")
else()
    message(STATUS "nxvc version: ${NXVC_VERSION_FULL} "
                   "(project() fallback; no tag reachable)")
endif()

# ---------------------------------------------------------------------------
# Generate the header.
# ---------------------------------------------------------------------------
if(NXVC_VERSION_IS_DIRTY)
    set(NXVC_VERSION_DIRTY_INT 1)
else()
    set(NXVC_VERSION_DIRTY_INT 0)
endif()

set(_nxvc_version_h "${NXWARP_GENERATED_INCLUDE_DIR}/nxvc/version.h")
configure_file("${CMAKE_CURRENT_LIST_DIR}/version.h.in" "${_nxvc_version_h}" @ONLY)

# An INTERFACE target so a component can pick the generated header up with a
# single target_link_libraries(<tgt> PRIVATE nxwarp::version) -- no include
# path juggling.  The root also puts the directory on the include path for
# every subdirectory, so linking is a convenience, not a requirement.
add_library(nxvc_version_header INTERFACE)
add_library(nxwarp::version ALIAS nxvc_version_header)
target_include_directories(nxvc_version_header INTERFACE
    $<BUILD_INTERFACE:${NXWARP_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>)

function(nxwarp_version_summary)
    message(STATUS "  library version   : ${NXVC_VERSION_FULL}")
    message(STATUS "  bitstream version : ${NXWARP_BITSTREAM_VERSION}")
endfunction()
