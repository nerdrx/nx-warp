# vk/encoder -- the NX Warp GPU encoder (paper 3.6, passes E0..E5).
#
# Self-contained: it can be added by vk/CMakeLists.txt with
# add_subdirectory(encoder), or configured directly as its own project, which
# is what the encoder build dir (build-enc/) does today.
#
# One line per component below; other passes append to NXENC_SHADERS and to the
# source list as they land.

cmake_minimum_required(VERSION 3.22)
if(NOT DEFINED PROJECT_NAME)
  project(nxwarp_encoder LANGUAGES C CXX)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
  endif()
endif()

# ------------------------------------------------------------------- Vulkan
# Prefer the system loader and headers; fall back to the vendored headers under
# tools/local, which is what this developer box has.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
  find_library(NXENC_VULKAN_LIB NAMES vulkan vulkan-1)
  find_path(NXENC_VULKAN_INC vulkan/vulkan.h
    HINTS ${NXWARP_VULKAN_INCLUDE_DIR} $ENV{VULKAN_SDK}/include
    PATHS /usr/include)
  if(NOT NXENC_VULKAN_LIB OR NOT NXENC_VULKAN_INC)
    message(STATUS "nxwarp-encoder: no Vulkan loader/headers, skipping GPU targets")
    set(NXENC_NO_VULKAN ON)
  endif()
endif()

# --------------------------------------------------------------------- glslc
find_program(NXENC_GLSLC glslc
  HINTS ${ANDROID_NDK}/shader-tools/linux-x86_64
        ${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64
        $ENV{ANDROID_NDK_HOME}/shader-tools/linux-x86_64
  PATHS /usr/bin /usr/local/bin)
if(NOT NXENC_GLSLC)
  message(STATUS "nxwarp-encoder: glslc not found, skipping GPU targets")
  set(NXENC_NO_VULKAN ON)
endif()

# ------------------------------------------------- CPU models (always built)
# The bit-exact reference for the analysis kernels.  No Vulkan dependency, so
# rc/ and the reference codec can link it without pulling in a loader.
add_library(nxenc_stats_cpu STATIC stats/stats_cpu.c)
target_include_directories(nxenc_stats_cpu PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/stats)
set_target_properties(nxenc_stats_cpu PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NXENC_NO_VULKAN)
  return()
endif()

# -------------------------------------------------------------------- shaders
set(NXENC_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/stats)
set(NXENC_GEN_DIR    ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${NXENC_GEN_DIR})

set(NXENC_SPV_HEADERS)
function(nxenc_shader name src defs)
  set(out ${NXENC_GEN_DIR}/${name}.spv.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${NXENC_GLSLC}
            -DSRC=${NXENC_SHADER_DIR}/${src}
            -DOUT=${out}
            -DNAME=${name}
            -DINCDIR=${NXENC_SHADER_DIR}
            -DDEFS=${defs}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_spv.cmake
    DEPENDS ${NXENC_SHADER_DIR}/${src}
            ${NXENC_SHADER_DIR}/nxe_common.glsl
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_spv.cmake
    COMMENT "glslc ${src} [${defs}] -> ${name}.spv.h"
    VERBATIM)
  set(NXENC_SPV_HEADERS ${NXENC_SPV_HEADERS} ${out} PARENT_SCOPE)
endfunction()

# E0: one variant per (import format x chroma mode).
nxenc_shader(E0_convert_rgba8_444   E0_convert.comp "-DNXE_SRC_FMT=0 -DNXE_CHROMA_420=0")
nxenc_shader(E0_convert_rgba8_420   E0_convert.comp "-DNXE_SRC_FMT=0 -DNXE_CHROMA_420=1")
nxenc_shader(E0_convert_rgb10a2_444 E0_convert.comp "-DNXE_SRC_FMT=1 -DNXE_CHROMA_420=0")
nxenc_shader(E0_convert_rgb10a2_420 E0_convert.comp "-DNXE_SRC_FMT=1 -DNXE_CHROMA_420=1")
# E0 YCbCr passthrough: the Linux compositor hands over a 2-plane 4:2:0 image
# (paper 3.6 runs the encoder on Monado's VkDevice, so we get its native
# format); the planes are bound as PLANE_0/PLANE_1 UINT views.
nxenc_shader(E0_convert_ycbcr8_420  E0_convert.comp "-DNXE_SRC_FMT=2 -DNXE_CHROMA_420=1 -DNXE_SRC_DEPTH10=0")
nxenc_shader(E0_convert_ycbcr10_420 E0_convert.comp "-DNXE_SRC_FMT=2 -DNXE_CHROMA_420=1 -DNXE_SRC_DEPTH10=1")
# E1: one pipeline, depth-independent (the tensor is accumulated undivided, so
# nothing in the kernel depends on the source bit depth).
nxenc_shader(E1_stats               E1_stats.comp   "")
# E2: block scan, single-block second level, add-offsets.
nxenc_shader(E2_prefix_p0           E2_prefix.comp  "-DNXE_E2_PASS=0")
nxenc_shader(E2_prefix_p1           E2_prefix.comp  "-DNXE_E2_PASS=1")
nxenc_shader(E2_prefix_p2           E2_prefix.comp  "-DNXE_E2_PASS=2")

add_custom_target(nxenc_shaders DEPENDS ${NXENC_SPV_HEADERS})

# ----------------------------------------------------------------- test tool
add_executable(nxvc-stats-test
  tools/nxvc-stats-test.cpp
  tools/vk_min.cpp
  ${NXENC_SPV_HEADERS})
add_dependencies(nxvc-stats-test nxenc_shaders)
target_include_directories(nxvc-stats-test PRIVATE
  ${NXENC_GEN_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/stats)
target_link_libraries(nxvc-stats-test PRIVATE nxenc_stats_cpu)
if(Vulkan_FOUND)
  target_link_libraries(nxvc-stats-test PRIVATE Vulkan::Vulkan)
else()
  target_include_directories(nxvc-stats-test PRIVATE ${NXENC_VULKAN_INC})
  target_link_libraries(nxvc-stats-test PRIVATE ${NXENC_VULKAN_LIB})
endif()

# ------------------------------------------------------------------- ctests
# When this directory is configured as its own project (build-enc/), wire up
# tests/vk-encoder so the encoder can be built and tested without the rest of
# the tree.  Configured from the repo root instead, the root adds that
# directory itself and this branch does not fire.
if(PROJECT_NAME STREQUAL "nxwarp_encoder" AND
   EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/vk-encoder/CMakeLists.txt)
  enable_testing()
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/vk-encoder
                   ${CMAKE_CURRENT_BINARY_DIR}/tests-vk-encoder)
endif()
