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
  # NXVC_VK_HEADERS_DIR is the variable the rest of vk/ takes (vk/common sets
  # it, and docs/NXWARP-E2E.md tells an integrator to pass it), so honour it
  # here too rather than making this one directory need its own spelling.
  find_path(NXENC_VULKAN_INC vulkan/vulkan.h
    HINTS ${NXWARP_VULKAN_INCLUDE_DIR} ${NXVC_VK_HEADERS_DIR} $ENV{VULKAN_SDK}/include
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

# The bit-exact CPU models of the coding kernels E3, E4 and E5.  Same rule: no
# Vulkan dependency, so the reference codec and the rate controller can link
# them, and `nxvc-vkenc --cpu` is a complete encoder on a machine with no GPU.
add_library(nxenc_forward_cpu STATIC
  forward/nxe_tables.c
  forward/forward_cpu.c
  forward/rans_cpu.c)
target_include_directories(nxenc_forward_cpu PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/forward)
set_target_properties(nxenc_forward_cpu PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NXENC_NO_VULKAN)
  return()
endif()

# -------------------------------------------------------------------- shaders
# One shader-build rule for the whole tree (vk/common/cmake); it compiles with
# -O0 and runs spirv-opt with the Adreno-safe pass list.  See
# docs/ADRENO-RULES.md.
set(NXENC_GEN_SPV ${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/nxvc_gen_spv.cmake)
set(NXENC_SPV_PASSES_CMAKE ${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake/NxvcShaderPasses.cmake)

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
            -DSTYLE=guarded
            -P ${NXENC_GEN_SPV}
    DEPENDS ${NXENC_SHADER_DIR}/${src}
            ${NXENC_SHADER_DIR}/nxe_common.glsl
            ${NXENC_GEN_SPV} ${NXENC_SPV_PASSES_CMAKE}
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

# E3/E4/E5 live in forward/ and take their variants as *specialization*
# constants rather than -D, so there is one SPIR-V module for each: the
# directional-intra switch and the transform edge are set at pipeline creation.
set(NXENC_FWD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/forward)
function(nxenc_fwd_shader name src)
  set(_fdefs "${ARGN}")
  set(out ${NXENC_GEN_DIR}/${name}.spv.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${NXENC_GLSLC}
            -DSRC=${NXENC_FWD_DIR}/${src}
            -DOUT=${out}
            -DNAME=${name}
            -DINCDIR=${NXENC_FWD_DIR}
            -DDEFS=${_fdefs}
            -DSTYLE=guarded
            -P ${NXENC_GEN_SPV}
    DEPENDS ${NXENC_FWD_DIR}/${src}
            ${NXENC_FWD_DIR}/nxe_enc_common.glsl
            ${NXENC_GEN_SPV} ${NXENC_SPV_PASSES_CMAKE}
    COMMENT "glslc ${src} -> ${name}.spv.h"
    VERBATIM)
  set(NXENC_SPV_HEADERS ${NXENC_SPV_HEADERS} ${out} PARENT_SCOPE)
endfunction()
nxenc_fwd_shader(E3_forward      forward.comp)
nxenc_fwd_shader(E4_rans_encode  rans_encode.comp)
nxenc_fwd_shader(E5_zero        packetize.comp "-DNXE_E5_PASS=0")
nxenc_fwd_shader(E5_packetize    packetize.comp "-DNXE_E5_PASS=1")

# ------------------------------------------------------------------- Pass W
#
# E0b is not a shader of this directory: it is the DECODER's `warp_pred.comp`,
# compiled from the decoder's own source with the decoder's own include tree.
# That is the whole point.  The encoder must never hold a reference the decoder
# cannot reproduce, and the cheapest way to guarantee that is for the predictor
# to be the same SPIR-V on both sides rather than two files that agree today.
# Copying the source here, or writing a second predictor against
# `inter_layout.h`, would both be re-derivations; see
# docs/adr/0028-gpu-inter-needs-an-integer-mode-decision.md and the E3b rule in
# README.md.
#
# glslc resolves quoted includes relative to the including file, so
# `warp_pred.comp`'s `#include "../passB/syntax_constants.h"` needs no -I; only
# `inter_layout.h` does.  The decoder's inter/ and passB/ must stay siblings,
# which is a constraint this rule now shares with vk/decoder/inter.
set(NXENC_DEC_INTER ${CMAKE_CURRENT_SOURCE_DIR}/../decoder/inter)
set(NXENC_DEC_PASSB ${CMAKE_CURRENT_SOURCE_DIR}/../decoder/passB)
if(EXISTS ${NXENC_DEC_INTER}/warp_pred.comp)
  set(_wout ${NXENC_GEN_DIR}/warp_pred.spv.h)
  add_custom_command(
    OUTPUT ${_wout}
    COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${NXENC_GLSLC}
            -DSRC=${NXENC_DEC_INTER}/warp_pred.comp
            -DOUT=${_wout}
            -DNAME=warp_pred
            -DINCDIR=${NXENC_DEC_INTER}
            -DSTYLE=guarded
            -P ${NXENC_GEN_SPV}
    DEPENDS ${NXENC_DEC_INTER}/warp_pred.comp
            ${NXENC_DEC_INTER}/inter_layout.h
            ${NXENC_DEC_PASSB}/syntax_constants.h
            ${NXENC_DEC_PASSB}/passB_layout.h
            ${NXENC_GEN_SPV} ${NXENC_SPV_PASSES_CMAKE}
    COMMENT "glslc (decoder) warp_pred.comp -> warp_pred.spv.h"
    VERBATIM)
  set(NXENC_SPV_HEADERS ${NXENC_SPV_HEADERS} ${_wout})
  set(NXENC_HAVE_PASSW 1)
else()
  set(NXENC_HAVE_PASSW 0)
  message(STATUS "vk/encoder: no vk/decoder/inter, Pass W not built")
endif()

add_custom_target(nxenc_shaders DEPENDS ${NXENC_SPV_HEADERS})

# ---------------------------------------------------- the inter host module
# The ring, the warp parameter buffer and the per-tile records Pass W reads.
# It includes the DECODER's inter_layout.h and syntax_constants.h rather than
# transcribing either, for the same reason the shader is the decoder's: two
# copies of a layout agree until they do not.
if(NXENC_HAVE_PASSW)
  add_library(nxenc_inter STATIC inter/nxe_inter.cpp)
  target_include_directories(nxenc_inter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inter
    ${NXENC_DEC_INTER})
  set_target_properties(nxenc_inter PROPERTIES CXX_STANDARD 20
                        CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)

  add_executable(nxenc-inter-test inter/nxe_inter_test.cpp)
  target_link_libraries(nxenc-inter-test PRIVATE nxenc_inter)
  set_target_properties(nxenc-inter-test PROPERTIES CXX_STANDARD 20
                        CXX_STANDARD_REQUIRED ON)
endif()

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

# ------------------------------------------------- the GPU encoder harness
# Links `ref/src/tables.cpp` for the built-in probability tables: those are a
# trained data set, not an algorithm, and a second copy in this tree would be a
# second thing to keep in step.  Everything else here is this directory's.
set(NXENC_REF_TABLES ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/src/tables.cpp)
if(EXISTS ${NXENC_REF_TABLES})
  add_executable(nxvc-vkenc
    tools/nxvc-vkenc.cpp
    tools/nxe_host.cpp
    tools/nxe_vk.cpp
    tools/nxe_e0.cpp
    tools/nxe_selftest.cpp
    tools/vk_min.cpp
    ${NXENC_REF_TABLES}
    ${NXENC_SPV_HEADERS})
  add_dependencies(nxvc-vkenc nxenc_shaders)
  target_include_directories(nxvc-vkenc PRIVATE
    ${NXENC_GEN_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/forward
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/stats
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/src)
  target_link_libraries(nxvc-vkenc PRIVATE nxenc_forward_cpu)
  set_target_properties(nxvc-vkenc PROPERTIES CXX_STANDARD 20)
  if(Vulkan_FOUND)
    target_link_libraries(nxvc-vkenc PRIVATE Vulkan::Vulkan)
  else()
    target_include_directories(nxvc-vkenc PRIVATE ${NXENC_VULKAN_INC})
    target_link_libraries(nxvc-vkenc PRIVATE ${NXENC_VULKAN_LIB})
  endif()
else()
  message(STATUS "nxwarp-encoder: ref/src/tables.cpp not found, no nxvc-vkenc")
endif()

# ----------------------------------------------------- the installable library
# nxvc_vk_encoder: the same encoder nxvc-vkenc drives, behind the C ABI in
# <nxvc/nxvc_vk_enc.h>, so a compositor can link it.  It is the encoder half of
# the pair nxvc_vk_decoder completes, and it is built from exactly the sources
# the harness uses -- no second copy of the driver, so the byte-identity the
# acid test pins is the byte-identity the library ships.
#
# Same ref/src/tables.cpp dependency and the same guard as nxvc-vkenc: without
# the reference's trained probability tables there is no encoder to install.
if(EXISTS ${NXENC_REF_TABLES})
  # The CPU model sources are compiled IN rather than linked: nxenc_forward_cpu
  # is an in-tree convenience target that is not installed, and a static
  # library that names it in its interface is one a consumer cannot link
  # (`cannot find -lnxenc_forward_cpu`).  An installed archive has to be
  # self-contained.
  add_library(nxvc_vk_encoder STATIC
    api/nxvc_vk_enc.cpp
    tools/nxe_host.cpp
    tools/nxe_vk.cpp
    tools/nxe_e0.cpp
    tools/vk_min.cpp
    forward/nxe_tables.c
    forward/forward_cpu.c
    forward/rans_cpu.c
    ${NXENC_REF_TABLES}
    ${NXENC_SPV_HEADERS})
  add_dependencies(nxvc_vk_encoder nxenc_shaders)
  target_include_directories(nxvc_vk_encoder
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
    PRIVATE
      ${NXENC_GEN_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/forward
      ${CMAKE_CURRENT_SOURCE_DIR}/tools
      ${CMAKE_CURRENT_SOURCE_DIR}/stats
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/src)
  set_target_properties(nxvc_vk_encoder PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
  if(Vulkan_FOUND)
    target_link_libraries(nxvc_vk_encoder PUBLIC Vulkan::Vulkan)
  else()
    target_include_directories(nxvc_vk_encoder PUBLIC ${NXENC_VULKAN_INC})
    target_link_libraries(nxvc_vk_encoder PUBLIC ${NXENC_VULKAN_LIB})
  endif()
  add_library(nxvc::vk_encoder ALIAS nxvc_vk_encoder)

  # The ABI's own harness: it exercises the library and nothing but the
  # library, which nxvc-vkenc does not.  tests/vk-encoder points acid.cmake at
  # it so the byte-identity claim covers the shipped surface.
  add_executable(nxvc-vkenc-api api/nxvc-vkenc-api.cpp)
  target_link_libraries(nxvc-vkenc-api PRIVATE nxvc_vk_encoder)
  # `--image` builds the compositor image itself, which needs the harness's
  # throwaway device layer -- the same one the library is built from, so this
  # is a header path and not a second copy.
  target_include_directories(nxvc-vkenc-api PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools)
  set_target_properties(nxvc-vkenc-api PROPERTIES CXX_STANDARD 20)
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
