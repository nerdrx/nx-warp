# nxwarp_cpack.cmake -- binary and source packaging.
#
# Generators: TGZ and ZIP always, DEB when dpkg is on the machine building the
# package.  There is no RPM generator: nothing in the target audience (WiVRn NX
# on Arch/Debian, the Pico client, Windows via mingw) asks for one, and an
# untested generator in the list is a broken release waiting to happen.
#
#   cmake --build build-release --target package         # binaries
#   cmake --build build-release --target package_source  # source tarball
#   cd build-release && cpack -G TGZ                     # one generator

include_guard(GLOBAL)

if(NOT NXWARP_INSTALL)
    return()
endif()

set(CPACK_PACKAGE_NAME "nxvc")
set(CPACK_PACKAGE_VENDOR "nerdrx")
set(CPACK_PACKAGE_CONTACT "nerdrx <66845818+nerdrx@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/nerdrx/nx-warp")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "NX Warp (nxvc) - a Vulkan compute video codec built only for VR streaming")

set(CPACK_PACKAGE_VERSION "${NXVC_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${NXVC_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${NXVC_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${NXVC_VERSION_PATCH}")

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")

# The file name carries the git description, not just X.Y.Z, so an artifact
# built off a branch can never be confused for the tagged release.
string(REPLACE "+" "_" _nxwarp_pkg_ver "${NXVC_VERSION_FULL}")
set(CPACK_PACKAGE_FILE_NAME
    "nxvc-${_nxwarp_pkg_ver}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "nxvc")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_STRIP_FILES ON)

set(CPACK_GENERATOR "TGZ;ZIP")

# ---------------------------------------------------------------------------
# DEB, only where it can actually be produced.
# ---------------------------------------------------------------------------
find_program(NXWARP_DPKG dpkg)
if(NXWARP_DPKG)
    list(APPEND CPACK_GENERATOR "DEB")

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"NX Warp (nxvc) - Vulkan compute video codec for VR streaming
 Independent 64x64 tiles, pose-warped prediction, no IDR, no CPU on
 the hot path.  Ships the bit-exact CPU reference codec, the transport
 library, and the nxv-enc / nxv-dec / nxv-info command line tools.")
    # dpkg's own idea of the architecture beats CMAKE_SYSTEM_PROCESSOR: amd64,
    # not x86_64.
    execute_process(COMMAND "${NXWARP_DPKG}" --print-architecture
                    OUTPUT_VARIABLE _nxwarp_deb_arch
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_nxwarp_deb_arch)
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_nxwarp_deb_arch}")
    endif()
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
else()
    message(STATUS "cpack: dpkg not found, DEB generator disabled")
endif()

# ---------------------------------------------------------------------------
# Source package.  Everything a build needs, nothing a build produces.
# ---------------------------------------------------------------------------
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "nxvc-${_nxwarp_pkg_ver}-src")
set(CPACK_SOURCE_IGNORE_FILES
    "/\\\\.git/"
    "/\\\\.github/"
    "/build[^/]*/"
    "/\\\\.cache/"
    "/__pycache__/"
    "/\\\\.gradle/"
    "\\\\.yuv$"
    "\\\\.apk$"
    "\\\\.aab$"
    "\\\\.o$"
    "\\\\.a$"
    "compile_commands\\\\.json$")

include(CPack)
