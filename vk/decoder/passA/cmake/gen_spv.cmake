# Compile a Pass A shader to SPIR-V and wrap it as a C array.
# cmake -DGLSLC=... -DSRC=... -DOUT=... -DNAME=... -DINCDIR=... -P gen_spv.cmake

execute_process(
  COMMAND ${GLSLC} --target-env=vulkan1.1 -O -mfmt=c -I ${INCDIR}
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
  "#pragma once\n#include <cstdint>\n"
  "static const uint32_t ${NAME}_spv[] = ${_body};\n")
