# Compile one GLSL compute shader to SPIR-V and wrap it as a C array.
# Invoked at build time: cmake -DGLSLC=... -DSRC=... -DOUT=... -DNAME=... \
#                              -DINCDIR=... -DDEFS=... -P gen_spv.cmake
# DEFS is a space-separated list of -D flags (may be empty).
#
# NXB_SPV_PASSES (environment) overrides the optimisation applied after
# glslc: empty means none, otherwise a space-separated spirv-opt pass list.
# It exists to bisect vendor miscompilations; see bench/README.md.

set(_defs)
if(DEFS)
  string(REPLACE " " ";" _defs "${DEFS}")
endif()

# glslc's own -O is not used: the SPIR-V optimiser is applied explicitly below
# so that the pass list is visible and can be changed for one shader.
execute_process(
  COMMAND ${GLSLC} --target-env=vulkan1.1 -O0 -I ${INCDIR} ${_defs}
          -o ${OUT}.spv ${SRC}
  RESULT_VARIABLE _rc
  ERROR_VARIABLE  _err)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "glslc failed for ${SRC}:\n${_err}")
endif()

set(_passes "$ENV{NXB_SPV_PASSES}")
if(NOT DEFINED ENV{NXB_SPV_PASSES})
  set(_passes "NXB_DEFAULT")
endif()

# spirv-opt's own -O list, with BOTH redundancy-elimination steps removed.
#
# Why: redundancy-elimination common-subexpression-eliminates the duplicate
# OpAccessChain that a load and a store to the same shared-memory word each
# produce, so both end up using one pointer id. The result is valid SPIR-V --
# spirv-val accepts it and RADV (wave32 and wave64) and lavapipe are bit-exact
# on it -- but the Adreno 650 driver on the Pico 4 miscompiles Pass B's LDS
# transpose when it is fed that form: the word that comes back out of shared
# memory belongs to a different slot, while every input to it (the coefficient
# word, the dequantised coefficient, the row-pass result and both the store and
# the load address) is verified bit-exact by --selftest. Dropping this one pass
# is what makes the Phase 0 gate bit-exact on the target device; the driver's
# own compiler does its own redundancy elimination, so nothing is given up.
# Bisected pass by pass on device; see bench/README.md "Adreno and spirv-opt".
if(_passes STREQUAL "NXB_DEFAULT")
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
elseif(_passes STREQUAL "")
  set(_passlist)
else()
  string(REPLACE " " ";" _passlist "${_passes}")
endif()

if(_passlist)
  get_filename_component(_bindir ${GLSLC} DIRECTORY)
  find_program(_SPIRV_OPT spirv-opt HINTS ${_bindir} PATHS /usr/bin /usr/local/bin)
  if(NOT _SPIRV_OPT)
    message(FATAL_ERROR "spirv-opt not found (looked next to ${GLSLC} and on PATH)")
  endif()
  execute_process(
    COMMAND ${_SPIRV_OPT} ${_passlist} -o ${OUT}.opt ${OUT}.spv
    RESULT_VARIABLE _rc2 ERROR_VARIABLE _err2)
  if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR "spirv-opt failed for ${SRC}:\n${_err2}")
  endif()
  file(RENAME ${OUT}.opt ${OUT}.spv)
endif()

# -mfmt=c is a glslc feature, so re-emit the (possibly optimised) module
# through glslc's disassembler-free path by reading it back as hex.
file(READ ${OUT}.spv _hex HEX)
string(LENGTH "${_hex}" _len)
math(EXPR _words "${_len} / 8")
set(_body "{")
set(_i 0)
while(_i LESS _words)
  math(EXPR _off "${_i} * 8")
  string(SUBSTRING "${_hex}" ${_off} 8 _w)
  # SPIR-V is little-endian on disk; reassemble each 32-bit word.
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
  "static const uint32_t ${NAME}_spv[] = ${_body};\n")
