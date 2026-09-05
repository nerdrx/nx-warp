# mirror.cmake -- every constant the C header and its GLSL mirror share must
# agree.  Run by ctest as `cmake -P`.
#
# SPDX-License-Identifier: Apache-2.0
#
# `vk/encoder/forward/nxe_enc.h` is the E3/E4/E5 contract and
# `vk/encoder/forward/nxe_enc_common.glsl` is its GLSL mirror.  The header says
# the two "are kept in step by the static assertions in nxvc-vkenc.cpp" -- and
# they were not: no such assertion existed, and there is no way to write one,
# because nothing in the C++ translation unit can see a GLSL `#define`.
#
# The failure mode is not hypothetical and is not loud.  NXE_LANE_OPS_CAP is
# the stride of E4's per-lane operation scratch: the host sizes the buffer from
# the C header and the shader indexes it from the GLSL copy, so a disagreement
# makes every lane address another lane's slot.  Nothing warns, nothing
# crashes, and the stream is quietly wrong.  The same shape of drift -- a
# hand-kept second copy of a layout -- is what the 27-context TableSet segfault
# was.
#
# So the check is textual, which is the only place it can be: pull every
# `#define NXE_<name> <value>` out of both files and require the ones both
# define to be identical. A name only one file has is fine and expected -- the
# GLSL side has no use for NXE_STREAM_HEADER_BYTES, the C side has no
# workgroup declarations -- so this compares the intersection and then insists
# the intersection is big enough to be meaningful.
#
# Variables: CHDR, GHDR.

if(NOT CHDR OR NOT GHDR)
  message(FATAL_ERROR "mirror.cmake: CHDR and GHDR are required")
endif()

# Pull `#define NAME VALUE` into two parallel lists.  Object-like macros only:
# a function-like macro (NXE_OP_PACK and friends) has a `(` immediately after
# the name and is skipped, because comparing those textually would compare
# formatting rather than meaning.
function(read_defines path out_names out_values)
  file(STRINGS ${path} lines)
  set(names)
  set(values)
  foreach(line ${lines})
    if(line MATCHES "^[ \t]*#[ \t]*define[ \t]+(NXE_[A-Za-z0-9_]+)[ \t]+([^ \t].*)$")
      set(nm ${CMAKE_MATCH_1})
      set(vl ${CMAKE_MATCH_2})
      # Drop a trailing comment of either language, then trailing whitespace.
      string(REGEX REPLACE "/\\*.*$" "" vl "${vl}")
      string(REGEX REPLACE "//.*$" "" vl "${vl}")
      string(STRIP "${vl}" vl)
      # Whitespace inside an expression is formatting, not value.
      string(REGEX REPLACE "[ \t]+" "" vl "${vl}")
      if(NOT vl STREQUAL "")
        list(APPEND names ${nm})
        list(APPEND values "${vl}")
      endif()
    endif()
  endforeach()
  set(${out_names} ${names} PARENT_SCOPE)
  set(${out_values} "${values}" PARENT_SCOPE)
endfunction()

read_defines(${CHDR} cnames cvalues)
read_defines(${GHDR} gnames gvalues)

list(LENGTH cnames ncdef)
list(LENGTH gnames ngdef)
if(ncdef EQUAL 0 OR ngdef EQUAL 0)
  message(FATAL_ERROR "mirror.cmake: parsed no defines (${ncdef} C, ${ngdef} "
                      "GLSL) -- the check is not doing anything")
endif()

set(nshared 0)
set(bad)
math(EXPR last "${ncdef} - 1")
foreach(i RANGE ${last})
  list(GET cnames ${i} nm)
  list(FIND gnames ${nm} j)
  if(j EQUAL -1)
    continue()
  endif()
  list(GET cvalues ${i} cv)
  list(GET gvalues ${j} gv)
  math(EXPR nshared "${nshared} + 1")
  if(NOT cv STREQUAL gv)
    list(APPEND bad "${nm}: nxe_enc.h has ${cv}, the GLSL mirror has ${gv}")
  endif()
endforeach()

if(bad)
  string(REPLACE ";" "\n  " report "${bad}")
  message(FATAL_ERROR
          "the C header and its GLSL mirror disagree:\n  ${report}\n"
          "Both are hand-written copies of one contract; fix the copy that is "
          "wrong, do not relax this test.")
endif()

# A guard on the guard.  If a refactor renames the constants on one side, the
# intersection collapses to nothing and every comparison trivially passes --
# so require it to stay in the range the two files have actually shared.
if(nshared LESS 20)
  message(FATAL_ERROR
          "mirror.cmake: only ${nshared} constants are shared, which is too "
          "few to be checking anything.  Either the two files drifted apart "
          "by renaming, or the parser stopped matching.")
endif()

message(STATUS "${nshared} shared constants agree between nxe_enc.h and "
               "nxe_enc_common.glsl")
