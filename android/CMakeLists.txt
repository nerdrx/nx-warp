cmake_minimum_required(VERSION 3.22)
project(nxwarp_client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT ANDROID)
  message(FATAL_ERROR
    "android/CMakeLists.txt builds the Android client only. The host-side "
    "blaster is a separate project: android/tools/nxvc-blast.")
endif()

# ---------------------------------------------------------------- glslc
# The NDK ships one under shader-tools; a system glslc works just as well
# because SPIR-V is the same everywhere. Same lookup as bench/.
find_program(NXC_GLSLC glslc
  HINTS
    ${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}
    ${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64
    $ENV{ANDROID_NDK_HOME}/shader-tools/linux-x86_64
  PATHS /usr/bin /usr/local/bin)
if(NOT NXC_GLSLC)
  message(FATAL_ERROR "glslc not found (looked in the NDK shader-tools and on PATH)")
endif()
message(STATUS "nxwarp-client: glslc = ${NXC_GLSLC}")

set(NXC_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(NXC_GEN_DIR    ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${NXC_GEN_DIR})

set(NXC_SPV_HEADERS)
function(nxc_shader name src)
  set(out ${NXC_GEN_DIR}/${name}.spv.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${NXC_GLSLC}
            -DSRC=${NXC_SHADER_DIR}/${src}
            -DOUT=${out}
            -DNAME=${name}
            -DINCDIR=${NXC_SHADER_DIR}
            -DDEFS=
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_spv.cmake
    DEPENDS ${NXC_SHADER_DIR}/${src} ${NXC_SHADER_DIR}/nxc_common.glsl
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_spv.cmake
    COMMENT "glslc ${src} -> ${name}.spv.h"
    VERBATIM)
  set(NXC_SPV_HEADERS ${NXC_SPV_HEADERS} ${out} PARENT_SCOPE)
endfunction()

nxc_shader(tiles   tiles.comp)     # placeholder decoder Pass B
nxc_shader(present present.comp)   # 2-plane 4:2:0 YCbCr -> RGBA
nxc_shader(hud     hud.comp)       # bitmap-font overlay

add_custom_target(nxc_shaders DEPENDS ${NXC_SPV_HEADERS})

# ---------------------------------------------------------------- library
add_library(nxwarpclient SHARED
  src/nxc_app.cpp
  src/nxc_util.cpp
  src/nxc_net.cpp
  src/nxc_font.cpp
  src/nxc_frame_ring.cpp
  src/nxc_transport_stub.cpp
  src/nxc_decoder_placeholder.cpp
  src/nxc_vk.cpp
  ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

add_dependencies(nxwarpclient nxc_shaders)

target_include_directories(nxwarpclient PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${NXC_GEN_DIR}
  ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue)

target_compile_options(nxwarpclient PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# NativeActivity resolves this symbol out of the .so by name.
target_link_options(nxwarpclient PRIVATE -u ANativeActivity_onCreate)

target_link_libraries(nxwarpclient PRIVATE
  vulkan
  android
  log)

# ---------------------------------------------------------------- transport/
#
# docs/TRANSPORT.md describes the nxvc_transport library that owns the wire
# format, the RS FEC and the feedback generator. It is being written in
# parallel. When it exists, this block links it and src/nxc_transport_stub.cpp
# is deleted; see the swap procedure at the top of src/nxc_transport.h.
#
# if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../transport/CMakeLists.txt)
#   add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../transport
#                    ${CMAKE_CURRENT_BINARY_DIR}/transport)
#   target_link_libraries(nxwarpclient PRIVATE nxvc_transport)
#   target_compile_definitions(nxwarpclient PRIVATE NXC_HAVE_TRANSPORT=1)
# endif()
