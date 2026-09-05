# Compile reconstruct.comp to SPIR-V and wrap it as a C array.
# cmake -DGLSLC=... -DSRC=... -DOUT=... -DNAME=... -DINCDIR=... -P gen_spv.cmake
#
# glslc's own -O is not used; the SPIR-V optimiser runs explicitly below so the
# pass list is visible and one pass can be left out of it.

if(NOT DEFINED DEFINES)
  set(DEFINES "")
endif()
separate_arguments(_defs NATIVE_COMMAND "${DEFINES}")

execute_process(
  COMMAND ${GLSLC} --target-env=vulkan1.1 -O0 -I ${INCDIR} ${_defs}
          -o ${OUT}.spv ${SRC}
  RESULT_VARIABLE _rc
  ERROR_VARIABLE  _err)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "glslc failed for ${SRC}:\n${_err}")
endif()

  # spirv-opt's -O list with BOTH redundancy-elimination steps removed.
  #
  # redundancy-elimination CSEs the duplicate OpAccessChain that a load and a
  # store to the same shared-memory word each produce, leaving one pointer id
  # shared by both. That is valid SPIR-V -- spirv-val accepts it, and RADV
  # (wave32 and wave64) and lavapipe are bit-exact on it -- but the Adreno 650
  # driver on the Pico 4 miscompiles shared-memory access in that form: the
  # word read back belongs to a different slot. Found and bisected pass by pass
  # on device against bench/'s Pass B; see bench/README.md "Adreno and
  # spirv-opt". This shader has the same shape (load then store through the
  # same index expression), so it takes the same pass list.
  #
  # Nothing is given up: the Adreno compiler does its own redundancy
  # elimination on the SPIR-V it receives.
  set(_passlist
    --wrap-opkill --eliminate-dead-branches --merge-return
    --inline-entry-points-exhaustive --eliminate-dead-functions
    --eliminate-dead-code-aggressive --private-to-local
    --eliminate-local-single-block --eliminate-local-single-store
    --eliminate-dead-code-aggressive --scalar-replacement=0
    --convert-local-access-chains --eliminate-local-single-block
    --eliminate-local-single-store --eliminate-dead-code-aggressive
    --ssa-rewrite --eliminate-dead-code-aggressive --ccp
    --eliminate-dead-code-aggressive --loop-unroll --eliminate-dead-branches
    --combine-access-chains --simplify-instructions --scalar-replacement=0
    --convert-local-access-chains --eliminate-local-single-block
    --eliminate-local-single-store --eliminate-dead-code-aggressive
    --ssa-rewrite --eliminate-dead-code-aggressive --vector-dce
    --eliminate-dead-inserts --eliminate-dead-branches --simplify-instructions
    --if-conversion --copy-propagate-arrays --reduce-load-size
    --eliminate-dead-code-aggressive --merge-blocks
    --eliminate-dead-branches --merge-blocks --simplify-instructions)

# NXVC_SPV_PASSES overrides the list, exactly as NXB_SPV_PASSES does in
# bench/: empty for no optimisation at all, "-O" to reproduce the
# redundancy-elimination miscompilation.  It exists so a suspected driver
# miscompile can be bisected against the pass list without editing this file.
if(DEFINED ENV{NXVC_SPV_PASSES})
  separate_arguments(_passlist NATIVE_COMMAND "$ENV{NXVC_SPV_PASSES}")
endif()

get_filename_component(_bindir ${GLSLC} DIRECTORY)
find_program(_SPIRV_OPT spirv-opt HINTS ${_bindir} PATHS /usr/bin /usr/local/bin)
if(NOT _SPIRV_OPT)
  message(FATAL_ERROR "spirv-opt not found (looked next to ${GLSLC} and on PATH)")
endif()
if(_passlist)
  execute_process(
    COMMAND ${_SPIRV_OPT} ${_passlist} -o ${OUT}.opt ${OUT}.spv
    RESULT_VARIABLE _rc2 ERROR_VARIABLE _err2)
  if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR "spirv-opt failed for ${SRC}:\n${_err2}")
  endif()
  file(RENAME ${OUT}.opt ${OUT}.spv)
endif()

# Re-emit as a C array (glslc's -mfmt=c is not available on a raw module).
file(READ ${OUT}.spv _hex HEX)
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

file(REMOVE ${OUT}.spv)
file(WRITE ${OUT}
  "// generated from ${SRC} -- do not edit\n"
  "#pragma once\n#include <cstdint>\n"
  "static const uint32_t ${NAME}_spv[] = ${_body};\n")
