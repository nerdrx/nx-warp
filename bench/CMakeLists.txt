cmake_minimum_required(VERSION 3.22)
project(nxwarp_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------- glslc
# The NDK ships one under shader-tools; a system glslc works just as well
# because SPIR-V is the same everywhere.
find_program(NXB_GLSLC glslc
  HINTS
    ${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}
    ${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64
    $ENV{ANDROID_NDK_HOME}/shader-tools/linux-x86_64
  PATHS /usr/bin /usr/local/bin)
if(NOT NXB_GLSLC)
  message(FATAL_ERROR "glslc not found (looked in the NDK shader-tools and on PATH)")
endif()
message(STATUS "nxwarp-bench: glslc = ${NXB_GLSLC}")

set(NXB_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(NXB_GEN_DIR    ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${NXB_GEN_DIR})

set(NXB_SPV_HEADERS)
function(nxb_shader name src defs)
  set(out ${NXB_GEN_DIR}/${name}.spv.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${NXB_GLSLC}
            -DSRC=${NXB_SHADER_DIR}/${src}
            -DOUT=${out}
            -DNAME=${name}
            -DINCDIR=${NXB_SHADER_DIR}
            -DDEFS=${defs}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_spv.cmake
    DEPENDS ${NXB_SHADER_DIR}/${src} ${NXB_SHADER_DIR}/nxb_common.glsl
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_spv.cmake
    COMMENT "glslc ${src} -> ${name}.spv.h"
    VERBATIM)
  set(NXB_SPV_HEADERS ${NXB_SPV_HEADERS} ${out} PARENT_SCOPE)
endfunction()

nxb_shader(k1_copy     k1_copy.comp     "")
nxb_shader(k2_warp     k2_warp.comp     "")
nxb_shader(k2b_sampler k2b_sampler.comp "")
nxb_shader(k4_rans     k4_rans.comp     "")
nxb_shader(k6_passc    k6_passc.comp    "")
nxb_shader(reproject   reproject.comp   "")
# K1 variants for the bandwidth sweep (--k1-sweep). One source, one -D set
# each; see shaders/k1_sweep.comp and src/core/nxb_k1sweep.cpp.
nxb_shader(k1v_base   k1_sweep.comp "-DNXB_K1_STORE=0 -DNXB_K1_WGX=8 -DNXB_K1_WGY=8")
nxb_shader(k1v_64x1   k1_sweep.comp "-DNXB_K1_STORE=0 -DNXB_K1_WGX=64 -DNXB_K1_WGY=1")
nxb_shader(k1v_16x16  k1_sweep.comp "-DNXB_K1_STORE=0 -DNXB_K1_WGX=16 -DNXB_K1_WGY=16")
nxb_shader(k1v_tiled  k1_sweep.comp "-DNXB_K1_STORE=0 -DNXB_K1_WGX=64 -DNXB_K1_WGY=1 -DNXB_K1_TILED=1")
nxb_shader(k1v_r32ui  k1_sweep.comp "-DNXB_K1_STORE=1 -DNXB_K1_WGX=64 -DNXB_K1_WGY=1")
nxb_shader(k1v_ssbo   k1_sweep.comp "-DNXB_K1_STORE=2 -DNXB_K1_WGX=64 -DNXB_K1_WGY=1")
nxb_shader(k1v_ssbo88 k1_sweep.comp "-DNXB_K1_STORE=2 -DNXB_K1_WGX=8 -DNXB_K1_WGY=8")
nxb_shader(k1v_unorm  k1_sweep.comp "-DNXB_K1_STORE=3 -DNXB_K1_WGX=64 -DNXB_K1_WGY=1")
nxb_shader(k1v_unorm88 k1_sweep.comp "-DNXB_K1_STORE=3 -DNXB_K1_WGX=8 -DNXB_K1_WGY=8")
nxb_shader(k1v_r32ui88 k1_sweep.comp "-DNXB_K1_STORE=1 -DNXB_K1_WGX=8 -DNXB_K1_WGY=8")
nxb_shader(k1v_ssbo1616 k1_sweep.comp "-DNXB_K1_STORE=2 -DNXB_K1_WGX=16 -DNXB_K1_WGY=16")

nxb_shader(passb_k3    passb.comp       "-DNXB_PREDICT=0")
nxb_shader(passb_k5    passb.comp       "-DNXB_PREDICT=1")

# One roll-up header so the core has a single include.
set(NXB_SPV_ALL ${NXB_GEN_DIR}/nxb_spv.h)
set(_incs "// generated -- do not edit\n#pragma once\n#include <cstdint>\n")
foreach(h ${NXB_SPV_HEADERS})
  get_filename_component(_n ${h} NAME)
  string(APPEND _incs "#include \"${_n}\"\n")
endforeach()
file(GENERATE OUTPUT ${NXB_SPV_ALL} CONTENT "${_incs}")

add_custom_target(nxb_shaders DEPENDS ${NXB_SPV_HEADERS} ${NXB_SPV_ALL})

# ---------------------------------------------------------------- core
add_library(nxbench_core STATIC
  src/core/nxb_vk.cpp
  src/core/nxb_bench.cpp
  src/core/nxb_runner.cpp
  src/core/nxb_report.cpp
  src/core/nxb_rans.cpp
  src/core/nxb_verify.cpp
  src/core/nxb_k1sweep.cpp)
add_dependencies(nxbench_core nxb_shaders)
target_include_directories(nxbench_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${NXB_GEN_DIR})
target_compile_options(nxbench_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

if(ANDROID)
  target_link_libraries(nxbench_core PUBLIC vulkan log android)
  target_compile_definitions(nxbench_core PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
else()
  # Prefer a proper SDK install; fall back to headers-only plus the loader,
  # which is the usual state of a machine that has drivers but no SDK package.
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    target_link_libraries(nxbench_core PUBLIC Vulkan::Vulkan)
  else()
    find_path(NXB_VK_INCLUDE vulkan/vulkan.h
      HINTS ${NXB_VULKAN_HEADERS}
            $ENV{VULKAN_SDK}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/Vulkan-Headers-1.4.309/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vulkan-headers-src/include
      PATHS /usr/include /usr/local/include)
    find_library(NXB_VK_LIB NAMES vulkan vulkan-1
      HINTS $ENV{VULKAN_SDK}/lib PATHS /usr/lib /usr/lib64 /usr/local/lib)
    if(NOT NXB_VK_INCLUDE OR NOT NXB_VK_LIB)
      message(FATAL_ERROR
        "Vulkan not found. Install a Vulkan SDK, or point -DNXB_VULKAN_HEADERS=<dir> "
        "at a directory containing vulkan/vulkan.h (headers found: ${NXB_VK_INCLUDE}, "
        "loader found: ${NXB_VK_LIB})")
    endif()
    message(STATUS "nxwarp-bench: vulkan headers ${NXB_VK_INCLUDE}, loader ${NXB_VK_LIB}")
    target_include_directories(nxbench_core PUBLIC ${NXB_VK_INCLUDE})
    target_link_libraries(nxbench_core PUBLIC ${NXB_VK_LIB})
  endif()
endif()

# ---------------------------------------------------------------- frontends
if(ANDROID)
  add_library(nxbench SHARED
    src/android/main.cpp
    src/android/k6_hybrid.cpp
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  target_include_directories(nxbench PRIVATE
    ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue)
  target_link_libraries(nxbench PRIVATE nxbench_core
    vulkan log android nativewindow mediandk)
  target_link_options(nxbench PRIVATE -u ANativeActivity_onCreate)
else()
  add_executable(nxbench-host src/host/main.cpp)
  target_link_libraries(nxbench-host PRIVATE nxbench_core)
endif()
