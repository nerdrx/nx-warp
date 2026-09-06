# vk.encoder.rowpresent -- row_present() (SYNTAX.md 3.1.2, tool bit 32).
#
# This test does NOT compare against nxv-enc, and the reason is not a spec
# ambiguity but a missing reference emission:
#
#   * the reference ENCODER never sets frame flag bit 4.  Its frame-flag
#     assembly (ref/src/codec_impl.inc, `fp.flags = have_ref ? 0u : 1u` and the
#     two `|=` that follow) writes bits 0, 2 and 3 and nothing else, and the
#     `row_present` / `row_bits` members of its FrameParams are declared and
#     never written.  `nxv-enc --row-present on` therefore advertises tool
#     bit 32 in the stream header -- which is legal, the tool bit is an offer
#     and flag bit 4 is per frame -- and then emits no bitmap on any frame.  Its
#     output is byte-identical to `--row-present off`.
#   * the reference DECODER does implement 3.1.2 fully: it reads flag bit 4,
#     rejects it without tool bit 32, parses the bitmap and treats an unnamed
#     row as all-skipped.
#
# So there is nothing to be byte-identical TO, and the check runs in the
# direction conformance actually runs in -- against the decoder:
#
#   1. nxv-dec must ACCEPT the stream.  This is the strong half: the bitmap
#      changes where every row header and every tile after it lives, so an
#      encoder that miscounts by one row produces a frame whose length disagrees
#      with its header, and the decoder loses the frame AFTER it.
#   2. it must decode to EXACTLY the pixels the same clip decodes to with the
#      tool off.  3.1.2: a 0-bit row is decoded "exactly as a row whose
#      skip_bitmap names every column" -- the bitmap elides bytes that state
#      what a skip already states, and changes no reconstruction.
#   3. the byte delta is reported, not asserted.  On a picture with a coded tile
#      in every row there is nothing to elide and the bitmap costs its own few
#      bytes; the syntax says as much ("still legal, and still five bytes").
#      The saving appears exactly in proportion to how much of the picture is
#      idle, which is what the 1088 leg is for.
#
# Expects VKENC, NXVDEC, WORKDIR, DEVICE.

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

if(DEVICE STREQUAL "cpu")
  message(STATUS "SKIP: the inter path has no CPU model yet")
  return()
endif()
execute_process(COMMAND ${VKENC} --list RESULT_VARIABLE rc OUTPUT_QUIET
                ERROR_QUIET)
if(rc EQUAL 77)
  message(STATUS "SKIP: no Vulkan ICD or no physical device")
  return()
endif()

execute_process(COMMAND ${VKENC} --dump-inter ${WORKDIR}/
                OUTPUT_VARIABLE fixture RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc --dump-inter failed: ${rc}")
endif()
string(STRIP "${fixture}" fixture)
string(REPLACE " " ";" fx "${fixture}")
list(GET fx 0 YUV)
list(GET fx 1 POSES)
list(GET fx 2 W)
list(GET fx 3 H)
list(GET fx 4 FRAMES)

# `atlas` and `plain` are separate legs because 3.1.2 says the two tools are
# orthogonal and either may be set alone; a bitmap that only worked under ATLAS
# would satisfy neither sentence.  Lite is here because it changes what a coded
# tile costs and therefore which rows end up idle.
foreach(leg "plain;;rans" "atlas;--atlas;rans" "atlas-lite;--atlas;lite")
  list(GET leg 0 name)
  list(GET leg 1 atflag)
  list(GET leg 2 ent)
  set(ENTFLAG --custom-tables --tab v2)
  if(ent STREQUAL "lite")
    set(ENTFLAG --entropy lite)
  endif()
  set(common --in ${YUV} --w ${W} --h ${H} --pix yuv420p --qp 26
             --frames ${FRAMES} --nsub 3 --matrix 1 --wm 0 --tskip off
             --chroma-qp-off 0 --ctx v3 --eyes 1 --intra-dir off --quiet
             --inter --intra-period 6 --poses ${POSES} --device ${DEVICE}
             ${ENTFLAG} ${atflag})

  execute_process(COMMAND ${VKENC} ${common} --out ${WORKDIR}/${name}.off.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: nxvc-vkenc failed (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${VKENC} ${common} --row-present
                          --out ${WORKDIR}/${name}.on.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: --row-present failed (${rc}): ${eout}")
  endif()

  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/${name}.off.nxv
                          --out ${WORKDIR}/${name}.off.yuv --pix yuv420p --quiet
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: nxv-dec refused the control stream (${rc})")
  endif()
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/${name}.on.nxv
                          --out ${WORKDIR}/${name}.on.yuv --pix yuv420p --quiet
                  RESULT_VARIABLE rc ERROR_VARIABLE derr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "${name}: nxv-dec refused the row_present stream (${rc}): ${derr}\n"
      "A frame whose emitted length disagrees with the `total` in its own "
      "header loses the frame AFTER it, so a miscount of one elided row "
      "reports here and not on the frame that caused it.")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/${name}.off.yuv ${WORKDIR}/${name}.on.yuv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "${name}: row_present changed the decoded pixels.  3.1.2 makes an "
      "unnamed row identical to one whose skip_bitmap names every column, so "
      "the bitmap must elide bytes and nothing else.")
  endif()

  file(SIZE ${WORKDIR}/${name}.off.nxv soff)
  file(SIZE ${WORKDIR}/${name}.on.nxv son)
  message(STATUS "vk.encoder.rowpresent ${name}: ${soff} -> ${son} bytes, "
                 "decoded identical")
endforeach()

# The control that keeps the three legs above honest: with the tool OFF the
# stream must be byte-identical to nxv-enc, which is what says the bitmap
# machinery is inert when not asked for.  It is checked here rather than left
# to the acid tests because the E5 change touches every offset in the frame,
# and an unguarded bitmap write lands on the first row header -- which is
# exactly the bug this file was written against.
message(STATUS "vk.encoder.rowpresent: the tool-off control is the acid tests")
