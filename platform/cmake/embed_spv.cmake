# cmake -P embed_spv.cmake -DIN=<file.spv> -DOUT=<file.h> -DSYMBOL=<name>
#
# Turns a SPIR-V module into a uint32_t array in a header. Keeping the shader
# inside the exe matters: the probe is copied to a bare Windows box as a single
# file.

if(NOT DEFINED IN OR NOT DEFINED OUT OR NOT DEFINED SYMBOL)
  message(FATAL_ERROR "embed_spv.cmake needs -DIN= -DOUT= -DSYMBOL=")
endif()

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hex_len)
math(EXPR byte_count "${hex_len} / 2")
math(EXPR rem "${byte_count} % 4")
if(NOT rem EQUAL 0)
  message(FATAL_ERROR "${IN} is ${byte_count} bytes, not a multiple of 4: not SPIR-V")
endif()

set(body "")
set(i 0)
set(col 0)
while(i LESS hex_len)
  # SPIR-V is little-endian on every target we build for.
  string(SUBSTRING "${hex}" ${i} 2 b0)
  math(EXPR n "${i} + 2")
  string(SUBSTRING "${hex}" ${n} 2 b1)
  math(EXPR n "${i} + 4")
  string(SUBSTRING "${hex}" ${n} 2 b2)
  math(EXPR n "${i} + 6")
  string(SUBSTRING "${hex}" ${n} 2 b3)
  string(APPEND body "0x${b3}${b2}${b1}${b0}u,")
  math(EXPR col "${col} + 1")
  if(col EQUAL 8)
    string(APPEND body "\n    ")
    set(col 0)
  else()
    string(APPEND body " ")
  endif()
  math(EXPR i "${i} + 8")
endwhile()

get_filename_component(in_name "${IN}" NAME)
file(WRITE "${OUT}"
"// Generated from ${in_name} by platform/cmake/embed_spv.cmake. Do not edit.
#pragma once

#include <cstdint>

namespace nxwarp::win {

inline constexpr uint32_t ${SYMBOL}[] = {
    ${body}
};

} // namespace nxwarp::win
")
