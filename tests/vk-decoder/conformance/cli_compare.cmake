# vk.decoder.cli -- nxvc-vkdec must be a drop-in for nxv-dec.
#
# The quality harness drives a decoder through --codec-cmd (tools/quality/
# README.md), so the GPU CLI has to take the same options and write the same
# planar bytes.  This checks both over every conformance vector.
#
# Skipping is reported by printing "no usable Vulkan ICD"; the ctest entry
# carries a SKIP_REGULAR_EXPRESSION for it, because a `cmake -P` script cannot
# choose its own exit code.

file(MAKE_DIRECTORY ${WORK})
# Only the decodable vectors: tests/vectors also holds r*.nxv, the rejection
# vectors, which both decoders are supposed to refuse.
file(GLOB vectors ${VECTORS}/v*.nxv)
list(LENGTH vectors n)
if(n EQUAL 0)
  message(FATAL_ERROR "no vectors under ${VECTORS}")
endif()
list(SORT vectors)

set(checked 0)
set(skipped 0)
foreach(v IN LISTS vectors)
  get_filename_component(name ${v} NAME_WE)
  # tests/vectors also holds the Phase 2 inter vectors.  docs/SYNTAX.md 12 says
  # a Phase 1 decoder refuses any stream whose `tools` mask carries a bit it
  # does not implement, so those are skipped here rather than compared --
  # detected from the stream header's own u64 `tools` field at byte 32, not
  # from the file name.
  file(READ ${v} hdr HEX LIMIT 40)
  string(SUBSTRING "${hdr}" 64 16 toolhex)   # bytes 32..39, little endian
  set(_mask 0)
  foreach(_b 0 1 2 3 4 5 6 7)
    math(EXPR _off "${_b} * 2")
    string(SUBSTRING "${toolhex}" ${_off} 2 _byte)
    math(EXPR _v "0x${_byte}")
    math(EXPR _mask "${_mask} + (${_v} << (8 * ${_b}))")
  endforeach()
  # Phase 1 set: bits 0-9, 17, 20, 21, 22.
  math(EXPR _p1 "0x3F03FF")
  math(EXPR _extra "${_mask} & ~${_p1}")
  if(NOT _extra EQUAL 0)
    math(EXPR skipped "${skipped} + 1")
    continue()
  endif()
  execute_process(COMMAND ${REFDEC} --in ${v} --out ${WORK}/${name}.ref.yuv
                          --quiet
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE reftxt)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-dec failed on ${name}: ${reftxt}")
  endif()
  execute_process(COMMAND ${VKDEC} --in ${v} --out ${WORK}/${name}.gpu.yuv
                          --quiet
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE gputxt)
  if(rc EQUAL 77)
    message(STATUS "no usable Vulkan ICD: ${gputxt}")
    file(REMOVE_RECURSE ${WORK})
    return()
  endif()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkdec failed on ${name}: ${gputxt}")
  endif()
  file(MD5 ${WORK}/${name}.ref.yuv a)
  file(MD5 ${WORK}/${name}.gpu.yuv b)
  if(NOT a STREQUAL b)
    message(FATAL_ERROR "${name}: nxvc-vkdec output differs from nxv-dec")
  endif()
  math(EXPR checked "${checked} + 1")
endforeach()
message(STATUS "${checked} vector(s): nxvc-vkdec is byte-identical to nxv-dec"
               " (${skipped} Phase 2 vector(s) skipped)")
file(REMOVE_RECURSE ${WORK})
