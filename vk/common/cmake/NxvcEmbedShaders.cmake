# NxvcEmbedShaders.cmake - compile GLSL compute shaders to SPIR-V and embed
# them as C++ headers.
#
# docs/PAPER.md 3.10: "GLSL 4.60, Vulkan semantics, glslang to SPIR-V at build
# time, embedded as arrays".  There is no runtime shader loading anywhere in
# the codec: the Android client ships one .so and the SPIR-V is inside it.
#
# Finds a compiler once, in this order:
#   1. NXVC_GLSLC / NXVC_GLSLANG_VALIDATOR cache variables, if the user set them
#   2. glslc and glslangValidator on PATH
#   3. the Android NDK's shader-tools/<host>/glslc, when ANDROID_NDK is known
#
# Usage:
#   nxvc_add_shaders(<target>
#     OUT_VAR   <var receiving the generated header dir>
#     SHADERS   a.comp b.comp
#     [DEFINES  FOO=1 BAR]        # applied to every shader in this call
#     [SUFFIX   _clustered]       # appended to the generated symbol name
#   )
# Each `a.comp` becomes `<gen>/nxvc/vk/shaders/a<SUFFIX>.h` declaring
# `nxvc::vk::shaders::a<SUFFIX>_spv`.

include_guard(GLOBAL)

# The one spirv-opt pass list, shared with bench/, android/, warp/ and
# vk/encoder/.  Defines NXVC_SPIRV_SAFE_PASSES; see docs/ADRENO-RULES.md.
include("${CMAKE_CURRENT_LIST_DIR}/NxvcShaderPasses.cmake")

find_program(NXVC_SPIRV_OPT NAMES spirv-opt
  HINTS
    $ENV{VULKAN_SDK}/bin
    ${ANDROID_NDK}/shader-tools/linux-x86_64
    ${ANDROID_NDK}/shader-tools/darwin-x86_64
    ${ANDROID_NDK}/shader-tools/windows-x86_64
  DOC "SPIRV-Tools spirv-opt")

find_program(NXVC_GLSLC NAMES glslc
  HINTS
    $ENV{VULKAN_SDK}/bin
    ${ANDROID_NDK}/shader-tools/linux-x86_64
    ${ANDROID_NDK}/shader-tools/darwin-x86_64
    ${ANDROID_NDK}/shader-tools/windows-x86_64
  DOC "Google shaderc glslc")
find_program(NXVC_GLSLANG_VALIDATOR NAMES glslangValidator glslang
  HINTS $ENV{VULKAN_SDK}/bin
  DOC "Khronos glslangValidator")

if(NXVC_GLSLC)
  set(NXVC_SHADER_COMPILER "${NXVC_GLSLC}" CACHE INTERNAL "")
  set(NXVC_SHADER_COMPILER_KIND "glslc" CACHE INTERNAL "")
elseif(NXVC_GLSLANG_VALIDATOR)
  set(NXVC_SHADER_COMPILER "${NXVC_GLSLANG_VALIDATOR}" CACHE INTERNAL "")
  set(NXVC_SHADER_COMPILER_KIND "glslang" CACHE INTERNAL "")
else()
  message(FATAL_ERROR
    "No GLSL compiler found.  Install glslc (shaderc) or glslangValidator, or "
    "set -DNXVC_GLSLC=/path/to/glslc.  The Android NDK ships one under "
    "shader-tools/<host>/glslc.")
endif()

# 3.10 asks for SPIR-V 1.4.  That needs either Vulkan 1.2 or the
# VK_KHR_spirv_1_4 extension on a 1.1 device, and the Pico 4's Adreno driver is
# a plain Vulkan 1.1 implementation, so the default target here is vulkan1.1
# (SPIR-V 1.3), which every device in the 3.7 table accepts.  Raise it with
# -DNXVC_VK_SPIRV_TARGET_ENV=vulkan1.2 when the device set allows.
set(NXVC_VK_SPIRV_TARGET_ENV "vulkan1.1" CACHE STRING
    "glslc --target-env for the codec shaders")
set(NXVC_VK_SHADER_OPTIMIZE ON CACHE BOOL
    "Run the SPIR-V optimiser on embedded shaders")

set(_NXVC_EMBED_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(nxvc_add_shaders TARGET)
  cmake_parse_arguments(ARG "" "OUT_VAR;SUFFIX" "SHADERS;DEFINES" ${ARGN})
  if(NOT ARG_SHADERS)
    message(FATAL_ERROR "nxvc_add_shaders(${TARGET}): no SHADERS given")
  endif()

  set(gen_root "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(gen_dir "${gen_root}/nxvc/vk/shaders")
  file(MAKE_DIRECTORY "${gen_dir}")

  set(define_flags "")
  foreach(d IN LISTS ARG_DEFINES)
    list(APPEND define_flags "-D${d}")
  endforeach()

  set(headers "")
  foreach(src IN LISTS ARG_SHADERS)
    get_filename_component(abs "${src}" ABSOLUTE)
    get_filename_component(name "${src}" NAME_WE)
    set(sym "${name}${ARG_SUFFIX}_spv")
    set(spv "${CMAKE_CURRENT_BINARY_DIR}/spv/${name}${ARG_SUFFIX}.spv")
    set(hdr "${gen_dir}/${name}${ARG_SUFFIX}.h")

    if(NXVC_SHADER_COMPILER_KIND STREQUAL "glslc")
      # -O0 always.  glslc's -O runs spirv-opt's built-in list, which contains
      # the redundancy-elimination pass the Adreno 650 driver miscompiles
      # (docs/ADRENO-RULES.md); optimisation happens in the separate spirv-opt
      # step below, from NXVC_SPIRV_SAFE_PASSES.
      set(compile_cmd
        "${NXVC_SHADER_COMPILER}" -fshader-stage=compute
        --target-env=${NXVC_VK_SPIRV_TARGET_ENV} -O0
        ${define_flags} -MD -MF "${spv}.d" -o "${spv}" "${abs}")
      set(depfile_args DEPFILE "${spv}.d")
    else()
      # glslangValidator: --target-env takes the same spellings.
      set(compile_cmd
        "${NXVC_SHADER_COMPILER}" -S comp --target-env ${NXVC_VK_SPIRV_TARGET_ENV}
        ${define_flags} -o "${spv}" "${abs}")
      set(depfile_args "")
    endif()

    # The optimiser runs as its own step, with the shared Adreno-safe pass
    # list, so that the pass set is visible and identical to every other
    # component's (vk/common/cmake/NxvcShaderPasses.cmake).
    set(opt_cmd "")
    if(NXVC_VK_SHADER_OPTIMIZE AND NXVC_SPIRV_OPT)
      set(opt_cmd COMMAND "${NXVC_SPIRV_OPT}" ${NXVC_SPIRV_SAFE_PASSES}
                          -o "${spv}" "${spv}")
    endif()

    add_custom_command(
      OUTPUT "${spv}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/spv"
      COMMAND ${compile_cmd}
      ${opt_cmd}
      DEPENDS "${abs}" "${_NXVC_EMBED_CMAKE_DIR}/NxvcShaderPasses.cmake"
      ${depfile_args}
      COMMENT "SPIR-V ${name}${ARG_SUFFIX} (${NXVC_VK_SPIRV_TARGET_ENV})"
      VERBATIM)

    add_custom_command(
      OUTPUT "${hdr}"
      COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${hdr} -DSYM=${sym}
              -P "${_NXVC_EMBED_CMAKE_DIR}/bin2h.cmake"
      DEPENDS "${spv}" "${_NXVC_EMBED_CMAKE_DIR}/bin2h.cmake"
      COMMENT "Embedding ${name}${ARG_SUFFIX}.h"
      VERBATIM)

    list(APPEND headers "${hdr}")
  endforeach()

  # One custom target per (target, suffix) pair, so a shader set can be
  # compiled more than once from the same source with different -D flags.
  set(stamp_target "${TARGET}_shaders${ARG_SUFFIX}")
  add_custom_target(${stamp_target} DEPENDS ${headers})
  add_dependencies(${TARGET} ${stamp_target})
  target_sources(${TARGET} PRIVATE ${headers})
  target_include_directories(${TARGET} PUBLIC $<BUILD_INTERFACE:${gen_root}>)
  if(ARG_OUT_VAR)
    set(${ARG_OUT_VAR} "${gen_root}" PARENT_SCOPE)
  endif()
endfunction()
