# NX Warp platform integration probes and shims.
#
# This directory is deliberately buildable on its own: the probes cross-compile
# for a foreign target (Windows via llvm-mingw today, Android later) and must
# not drag the host build of the reference codec along. Building the repo root
# does not build anything here.
#
#   cmake -S platform -B build-win \
#         -DCMAKE_TOOLCHAIN_FILE=platform/cmake/llvm-mingw-toolchain.cmake
#   cmake --build build-win -j4
#
# It also works as an add_subdirectory() of a larger project.

cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(nxwarp_platform VERSION 0.1.0 LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
  endif()
endif()

if(WIN32)
  add_subdirectory(win)
else()
  message(STATUS "nx-warp/platform: target system is ${CMAKE_SYSTEM_NAME}, "
                 "nothing to build (win/ needs a Windows target)")
endif()
