# nxvc_gen_spv.cmake -- compile one GLSL compute shader to SPIR-V, optimise it
# with the Adreno-safe pass list, and emit it in one of three shapes.
#
# The single shader-build rule for the whole tree.  It replaced one copy per
# component (bench/, vk/encoder/, android/, warp/), which had drifted: three of
# the four were still passing `glslc -O`, whose built-in pass list contains the
# redundancy-elimination pass that the Adreno 650 driver miscompiles.  See
# NxvcShaderPasses.cmake for that story and docs/ADRENO-RULES.md for the rest.
#
# Invoked at build time:
#
#   cmake -DGLSLC=<glslc> -DSRC=<in.comp> -DOUT=<out> -DNAME=<symbol>
#         [-DINCDIR=<dir>] [-DINCDIR2=<dir>] [-DDEFS="-DA=1 -DB"]
#         [-DSTYLE=plain|guarded|raw]
#         [-DTARGET_ENV=vulkan1.1] [-DSTAGE=compute]
#         -P vk/common/cmake/nxvc_gen_spv.cmake
#
# STYLE picks the output shape:
#   plain    (default)  static const uint32_t <NAME>_spv[] = {...};
#   guarded             the same, behind #pragma once + #include <stdint.h>
#   raw                 the .spv module itself, no wrapper
#
# The environment variable NXVC_SPV_PASSES overrides the pass list: empty means
# "no spirv-opt at all", otherwise a space-separated list.  It exists to bisect
# vendor miscompilations, which is how the excluded pass was found in the first
# place.  NXB_SPV_PASSES is honoured as well, because bench/README.md documents
# it under that name.

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/NxvcShaderPasses.cmake")

foreach(_req GLSLC SRC OUT NAME)
  if(NOT DEFINED ${_req})
    message(FATAL_ERROR "nxvc_gen_spv: -D${_req}= is required")
  endif()
endforeach()

if(NOT DEFINED STYLE OR STYLE STREQUAL "")
  set(STYLE "plain")
endif()
if(NOT DEFINED STAGE OR STAGE STREQUAL "")
  set(STAGE "compute")
endif()
if(DEFINED TARGET_ENV AND NOT TARGET_ENV STREQUAL "")
  set(NXVC_SPIRV_TARGET_ENV "${TARGET_ENV}")
endif()

set(_defs)
if(DEFS)
  string(REPLACE " " ";" _defs "${DEFS}")
endif()

set(_incs)
if(INCDIR)
  set(_incs -I "${INCDIR}")
  # A second include directory, for a shader that draws on two contracts --
  # vk/encoder/inter/E1c_decide.comp includes the decoder's inter_layout.h and
  # the encoder's nxe_enc_common.glsl.  Optional and additive: a caller that
  # passes only INCDIR gets exactly the command line it got before.
  if(INCDIR2)
    list(APPEND _incs -I "${INCDIR2}")
  endif()
endif()

if(STYLE STREQUAL "raw")
  set(_spv "${OUT}")
else()
  set(_spv "${OUT}.spv")
endif()
get_filename_component(_outdir "${_spv}" DIRECTORY)
if(_outdir)
  file(MAKE_DIRECTORY "${_outdir}")
endif()

# -O0 on purpose: the optimiser runs below, from a pass list we control.  See
# NxvcShaderPasses.cmake.
execute_process(
  COMMAND "${GLSLC}" -fshader-stage=${STAGE}
          --target-env=${NXVC_SPIRV_TARGET_ENV} -O0
          ${_incs} ${_defs} -o "${_spv}" "${SRC}"
  RESULT_VARIABLE _rc
  ERROR_VARIABLE  _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "glslc failed for ${SRC}:\n${_err}")
endif()

# ------------------------------------------------------------------ spirv-opt
set(_passes "NXVC_DEFAULT")
if(DEFINED ENV{NXVC_SPV_PASSES})
  set(_passes "$ENV{NXVC_SPV_PASSES}")
elseif(DEFINED ENV{NXB_SPV_PASSES})
  set(_passes "$ENV{NXB_SPV_PASSES}")
endif()

if(_passes STREQUAL "NXVC_DEFAULT")
  set(_passlist ${NXVC_SPIRV_SAFE_PASSES})
elseif(_passes STREQUAL "")
  set(_passlist)
else()
  string(REPLACE " " ";" _passlist "${_passes}")
endif()

if(_passlist)
  get_filename_component(_bindir "${GLSLC}" DIRECTORY)
  find_program(_SPIRV_OPT spirv-opt HINTS "${_bindir}"
               PATHS /usr/bin /usr/local/bin)
  if(NOT _SPIRV_OPT)
    message(FATAL_ERROR
      "spirv-opt not found (looked next to ${GLSLC} and on PATH).  It is not "
      "optional: glslc -O is unusable on Adreno, so the optimiser has to run "
      "separately or not at all.  Set NXVC_SPV_PASSES= (empty) to build "
      "unoptimised SPIR-V instead.")
  endif()
  execute_process(
    COMMAND "${_SPIRV_OPT}" ${_passlist} -o "${_spv}.opt" "${_spv}"
    RESULT_VARIABLE _rc2 ERROR_VARIABLE _err2)
  if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR "spirv-opt failed for ${SRC}:\n${_err2}")
  endif()
  file(RENAME "${_spv}.opt" "${_spv}")
endif()

if(STYLE STREQUAL "raw")
  return()
endif()

# ------------------------------------------------------------ C array wrapper
# glslc's -mfmt=c is not available on an already-optimised module, so the words
# are reassembled here.  SPIR-V is little-endian on disk.
file(READ "${_spv}" _hex HEX)
string(LENGTH "${_hex}" _len)
math(EXPR _words "${_len} / 8")
set(_body "{")
set(_i 0)
while(_i LESS _words)
  math(EXPR _off "${_i} * 8")
  string(SUBSTRING "${_hex}" ${_off} 8 _w)
  string(SUBSTRING "${_w}" 0 2 _b0)
  string(SUBSTRING "${_w}" 2 2 _b1)
  string(SUBSTRING "${_w}" 4 2 _b2)
  string(SUBSTRING "${_w}" 6 2 _b3)
  string(APPEND _body "0x${_b3}${_b2}${_b1}${_b0},")
  math(EXPR _i "${_i} + 1")
endwhile()
string(APPEND _body "}")

file(REMOVE "${_spv}")

set(_prologue "")
if(STYLE STREQUAL "guarded")
  set(_prologue "#pragma once\n#include <stdint.h>\n")
endif()
file(WRITE "${OUT}"
  "// generated from ${SRC} -- do not edit\n"
  "${_prologue}"
  "static const uint32_t ${NAME}_spv[] = ${_body};\n")
