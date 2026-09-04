# nxwarp_install.cmake -- install rules, the CMake package, and pkg-config.
#
# Components are landing in parallel and none of them owns its install rules
# yet, so this file discovers what exists rather than being told.  Everything
# here is driven from two lists:
#
#   NXWARP_INSTALL_LIBS   static libraries to export as nxvc::<name>
#   NXWARP_INSTALL_TOOLS  CLI executables to install into bin/
#
# A component that has adopted the convention (see RELEASE.md "What a
# component CMakeLists should provide") simply appends its target to
# NXWARP_EXPORT_TARGETS from its own file and needs nothing here.
#
# Consumers get:
#     find_package(nxvc CONFIG REQUIRED)
#     target_link_libraries(app PRIVATE nxvc::ref nxvc::transport)
# or, without CMake:
#     pkg-config --cflags --libs nxvc

include_guard(GLOBAL)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(NXWARP_INSTALL "Generate install rules for nxvc" ON)

if(NOT NXWARP_INSTALL)
    return()
endif()

set(NXWARP_EXPORT_NAME nxvcTargets)
set(NXWARP_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/nxvc")

# ---------------------------------------------------------------------------
# Make an existing target exportable.
#
# The component CMakeLists were written before there were install rules and
# use bare absolute source paths in target_include_directories(... PUBLIC ...).
# install(EXPORT) refuses those, correctly: the path does not exist on the
# consumer's machine.  Rather than editing files owned by other people mid
# flight, rewrite the property here into the BUILD_INTERFACE/INSTALL_INTERFACE
# form.  Remove this once every component does it itself -- it is a migration
# shim, not the design.
# ---------------------------------------------------------------------------
function(nxwarp_make_exportable tgt)
    get_target_property(_type ${tgt} TYPE)
    get_target_property(_dirs ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _dirs)
        set(_dirs "")
    endif()

    set(_fixed "")
    set(_needs_install_iface FALSE)
    foreach(_d IN LISTS _dirs)
        if(_d MATCHES "\\$<")
            # Already a generator expression; assume the author knew what for.
            list(APPEND _fixed "${_d}")
        else()
            list(APPEND _fixed "$<BUILD_INTERFACE:${_d}>")
            set(_needs_install_iface TRUE)
        endif()
    endforeach()
    if(_needs_install_iface)
        list(APPEND _fixed "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    endif()
    set_property(TARGET ${tgt} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_fixed}")
endfunction()

# ---------------------------------------------------------------------------
# Discover what got built.
# ---------------------------------------------------------------------------
set(_nxwarp_candidate_libs
    nxvc_ref nxvc_warp_ref nxvc_transport nxvc_rc nxvc_fov
    nxvc_stereo nxvc_hybrid nxvc_vk nxvc_vk_common nxvc_vk_encoder nxvc_vk_decoder
    nxvc_core nxvc_platform)

set(_nxwarp_candidate_tools
    nxv-enc nxv-dec nxv-info
    nxvc-warpsim nxvc-warpdiff nxvc-netsim nxvc-rcsim nxvc-stereosim
    nxvc-conform nxvc-diff nxvc-dump)

set(NXWARP_INSTALL_LIBS "")
foreach(_t IN LISTS _nxwarp_candidate_libs)
    if(TARGET ${_t})
        get_target_property(_imported ${_t} IMPORTED)
        get_target_property(_aliased ${_t} ALIASED_TARGET)
        if(NOT _imported AND NOT _aliased)
            list(APPEND NXWARP_INSTALL_LIBS ${_t})
        endif()
    endif()
endforeach()

set(NXWARP_INSTALL_TOOLS "")
foreach(_t IN LISTS _nxwarp_candidate_tools)
    if(TARGET ${_t})
        list(APPEND NXWARP_INSTALL_TOOLS ${_t})
    endif()
endforeach()

# Anything a component added itself.
if(NXWARP_EXPORT_TARGETS)
    list(APPEND NXWARP_INSTALL_LIBS ${NXWARP_EXPORT_TARGETS})
    list(REMOVE_DUPLICATES NXWARP_INSTALL_LIBS)
endif()

message(STATUS "install: libraries  ${NXWARP_INSTALL_LIBS}")
message(STATUS "install: tools      ${NXWARP_INSTALL_TOOLS}")

# ---------------------------------------------------------------------------
# nxvc:: aliases.
#
# The alias name drops the nxvc_ prefix: nxvc_ref -> nxvc::ref.  Components
# should declare their own alias (it is one line and it makes a typo'd
# dependency a configure error instead of a link error); this loop only fills
# in the ones that have not yet.
# ---------------------------------------------------------------------------
foreach(_t IN LISTS NXWARP_INSTALL_LIBS)
    string(REGEX REPLACE "^nxvc_" "" _short "${_t}")
    if(NOT TARGET nxvc::${_short})
        add_library(nxvc::${_short} ALIAS ${_t})
    endif()
    if(NOT TARGET nxwarp::${_short})
        add_library(nxwarp::${_short} ALIAS ${_t})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Install the targets.
# ---------------------------------------------------------------------------
foreach(_t IN LISTS NXWARP_INSTALL_LIBS)
    nxwarp_make_exportable(${_t})
endforeach()

if(NXWARP_INSTALL_LIBS)
    install(TARGETS ${NXWARP_INSTALL_LIBS}
            EXPORT ${NXWARP_EXPORT_NAME}
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

install(TARGETS nxvc_version_header EXPORT ${NXWARP_EXPORT_NAME})

if(NXWARP_INSTALL_TOOLS)
    install(TARGETS ${NXWARP_INSTALL_TOOLS}
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# ---------------------------------------------------------------------------
# Headers.
#
# Every component keeps its public headers under <component>/include/<prefix>/,
# with the prefix naming the library (nxvc/, nxrc/, nxfov/).  Installing the
# directory contents preserves that, so #include <nxvc/transport/wire.h> means
# the same thing against the install tree as against the source tree.
# ---------------------------------------------------------------------------
set(_nxwarp_header_roots "${CMAKE_SOURCE_DIR}/include")
foreach(_c IN LISTS NXWARP_COMPONENTS)
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${_c}/include")
        list(APPEND _nxwarp_header_roots "${CMAKE_SOURCE_DIR}/${_c}/include")
    endif()
endforeach()

foreach(_root IN LISTS _nxwarp_header_roots)
    if(IS_DIRECTORY "${_root}")
        install(DIRECTORY "${_root}/"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
                FILES_MATCHING
                PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inc")
    endif()
endforeach()

# The generated version header.
install(FILES "${NXWARP_GENERATED_INCLUDE_DIR}/nxvc/version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nxvc")

# ---------------------------------------------------------------------------
# CMake package: find_package(nxvc CONFIG)
# ---------------------------------------------------------------------------
install(EXPORT ${NXWARP_EXPORT_NAME}
        FILE ${NXWARP_EXPORT_NAME}.cmake
        NAMESPACE nxvc::
        DESTINATION "${NXWARP_CMAKE_INSTALL_DIR}")

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/nxvcConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/nxvcConfig.cmake"
    INSTALL_DESTINATION "${NXWARP_CMAKE_INSTALL_DIR}")

write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/nxvcConfigVersion.cmake"
    VERSION "${NXVC_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_BINARY_DIR}/nxvcConfig.cmake"
        "${CMAKE_BINARY_DIR}/nxvcConfigVersion.cmake"
        DESTINATION "${NXWARP_CMAKE_INSTALL_DIR}")

# Usable straight from the build tree, no install step, for the sibling
# WiVRn NX checkout: -Dnxvc_DIR=/path/to/build-release
export(EXPORT ${NXWARP_EXPORT_NAME}
       FILE "${CMAKE_BINARY_DIR}/${NXWARP_EXPORT_NAME}.cmake"
       NAMESPACE nxvc::)

# ---------------------------------------------------------------------------
# pkg-config
# ---------------------------------------------------------------------------
set(_nxvc_pc_libs "")
foreach(_t IN LISTS NXWARP_INSTALL_LIBS)
    set(_nxvc_pc_libs "${_nxvc_pc_libs} -l${_t}")
endforeach()
string(STRIP "${_nxvc_pc_libs}" NXVC_PC_LIBS)

configure_file("${CMAKE_CURRENT_LIST_DIR}/nxvc.pc.in"
               "${CMAKE_BINARY_DIR}/nxvc.pc" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/nxvc.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# ---------------------------------------------------------------------------
# Documents that travel with a binary package.
# ---------------------------------------------------------------------------
install(FILES
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/README.md"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/nxvc")
