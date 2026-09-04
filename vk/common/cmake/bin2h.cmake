# bin2h.cmake - turn a SPIR-V binary into a C++ header holding a uint32_t array.
#
# Invoked as:  cmake -DIN=<file.spv> -DOUT=<file.h> -DSYM=<identifier> -P bin2h.cmake
#
# The array is `inline constexpr`, so the header can be included in as many
# translation units as the encoder and decoder like without a definition
# clash, and std::span<const uint32_t> binds to it directly.

if(NOT DEFINED IN OR NOT DEFINED OUT OR NOT DEFINED SYM)
  message(FATAL_ERROR "bin2h.cmake needs -DIN= -DOUT= -DSYM=")
endif()

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR bytes "${hexlen} / 2")
math(EXPR rem "${bytes} % 4")
if(NOT rem EQUAL 0)
  message(FATAL_ERROR "${IN}: ${bytes} bytes is not a whole number of SPIR-V words")
endif()
math(EXPR words "${bytes} / 4")

set(body "")
set(i 0)
set(col 0)
while(i LESS words)
  math(EXPR off "${i} * 8")
  string(SUBSTRING "${hex}" ${off} 8 w)
  # SPIR-V is emitted little-endian by every toolchain we use; swap to the
  # host's word order for a uint32_t literal.
  string(SUBSTRING "${w}" 0 2 b0)
  string(SUBSTRING "${w}" 2 2 b1)
  string(SUBSTRING "${w}" 4 2 b2)
  string(SUBSTRING "${w}" 6 2 b3)
  string(APPEND body "0x${b3}${b2}${b1}${b0}u,")
  math(EXPR col "${col} + 1")
  if(col EQUAL 8)
    string(APPEND body "\n    ")
    set(col 0)
  else()
    string(APPEND body " ")
  endif()
  math(EXPR i "${i} + 1")
endwhile()

get_filename_component(in_name "${IN}" NAME)
set(header "// Generated from ${in_name} by bin2h.cmake.  Do not edit.
#pragma once

#include <cstdint>

namespace nxvc::vk::shaders {

inline constexpr uint32_t ${SYM}[] = {
    ${body}
};
inline constexpr unsigned ${SYM}_word_count = ${words};

}  // namespace nxvc::vk::shaders
")

get_filename_component(out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")
file(WRITE "${OUT}" "${header}")
