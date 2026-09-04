cmake_minimum_required(VERSION 3.25)
project(nxwarp VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Build hygiene, version discovery and instrumentation. These modules define
# no targets of their own beyond three INTERFACE ones (nxwarp::warnings,
# nxwarp::deterministic, nxwarp::version) and never reach into a component.
# See BUILDING.md; the presets in CMakePresets.json drive them.
include(nxwarp_options)
include(nxwarp_sanitizers)
include(nxwarp_version)

# Generated headers (nxvc/version.h) are on the include path for every
# component without any of them having to ask.
include_directories(${NXWARP_GENERATED_INCLUDE_DIR})

# Each component lives in its own directory with its own CMakeLists.txt and is
# built only when present, so the tree stays configurable while components land.
set(NXWARP_COMPONENTS ref warp transport rc fov stereo hybrid vk tools platform)
foreach(comp IN LISTS NXWARP_COMPONENTS)
  string(TOUPPER ${comp} COMP)
  if(comp STREQUAL "vk")
    option(NXWARP_BUILD_VK "Build the Vulkan encoder/decoder" OFF)
  else()
    option(NXWARP_BUILD_${COMP} "Build the ${comp} component" ON)
  endif()
endforeach()
option(NXWARP_BUILD_TESTS "Build tests" ON)

foreach(comp IN LISTS NXWARP_COMPONENTS)
  string(TOUPPER ${comp} COMP)
  if(NXWARP_BUILD_${COMP} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${comp}/CMakeLists.txt)
    add_subdirectory(${comp})
  endif()
endforeach()

# Tests: every tests/<name>/CMakeLists.txt is added directly, guarded on EXISTS,
# so a missing sibling never breaks configure. tests/CMakeLists.txt is NOT used.
if(NXWARP_BUILD_TESTS)
  enable_testing()
  file(GLOB _nxwarp_test_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/tests/*)
  foreach(dir IN LISTS _nxwarp_test_dirs)
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
      get_filename_component(_name ${dir} NAME)
      if(_name STREQUAL "vk" OR _name MATCHES "^vk-")
        if(NOT NXWARP_BUILD_VK)
          continue()
        endif()
      endif()
      add_subdirectory(${dir} ${CMAKE_CURRENT_BINARY_DIR}/tests/${_name})
    endif()
  endforeach()
endif()

# Install rules, the find_package(nxvc) package, pkg-config and CPack. These
# run last because they discover which component targets actually exist rather
# than being told; a component that has adopted the export convention (see
# RELEASE.md) is picked up automatically.
include(nxwarp_install)
include(nxwarp_cpack)

message(STATUS "--- nxwarp ---")
nxwarp_version_summary()
nxwarp_options_summary()
nxwarp_sanitizers_summary()
