# Compile one GLSL compute shader to SPIR-V and wrap it as a C array.
# Invoked at build time:
#   cmake -DGLSLC=... -DSRC=... -DOUT=... -DNAME=... -DINCDIR=... -DDEFS=... \
#         -P gen_spv.cmake
# DEFS is a space-separated list of -D flags (may be empty).
#
# The same shape as bench/cmake/gen_spv.cmake on purpose: when vk/common grows
# a shared shader-build rule these two collapse into it.

set(_defs)
if(DEFS)
  string(REPLACE " " ";" _defs "${DEFS}")
endif()

execute_process(
  COMMAND ${GLSLC} --target-env=vulkan1.1 -O -mfmt=c -I ${INCDIR} ${_defs}
          -o ${OUT}.body ${SRC}
  RESULT_VARIABLE _rc
  ERROR_VARIABLE  _err)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "glslc failed for ${SRC}:\n${_err}")
endif()

file(READ ${OUT}.body _body)
file(REMOVE ${OUT}.body)
file(WRITE ${OUT}
  "// generated from ${SRC} -- do not edit\n"
  "#pragma once\n"
  "#include <stdint.h>\n"
  "static const uint32_t ${NAME}_spv[] = ${_body};\n")
