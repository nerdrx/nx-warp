# vk.encoder.inter.cv1088 -- the coded-vector inter path at the headset's own
# tile count: 1088x1088, a 17x17 grid, 289 tiles.
#
# Every other inter fixture in this tree is 320x240 or smaller -- 20 tiles or
# fewer -- and 256 is exactly where a tile index stops fitting in a byte, where
# a row's skip bitmap stops being a quarter of its u64, and where the
# per-tile prefix sums the packetizer runs stop being small.  A syntax element
# that is wrong only above 256 tiles would have shipped, so this leg pins the
# whole coded-vector chain at 289:
#
#   1. the GPU encoder's stream is byte-identical to `nxv-enc` with
#      `--int-coded-vectors static`, which is the normative encoder;
#   2. `nxv-dec` -- the normative decoder -- accepts it and decodes it to the
#      same pixels as the reference stream;
#   3. the GPU decoder's host parser accepts the same stream and Pass A/B/W
#      reproduce `nxv-dec`'s picture sample for sample.  (3) is the leg that
#      would have caught a `tile_index`, `payload_len`, row-header or ring
#      check that only misbehaves past 256 tiles, because the parser is a
#      second implementation of the same walk;
#   4. the library ABI at `coded_vectors = DEFAULT` produces the same stream,
#      which is the entry point WiVRn links.
#
# The fixture is `nxvc-vkenc --dump-inter` at an explicit size, so the picture
# is the same description the 256x192 leg uses rather than a second one.
#
# Expects VKENC, NXVENC, NXVDEC, WORKDIR, DEVICE; optionally VKDEC, VKENCAPI.

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
                        --dump-inter-size 1088 1088 8
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

if(NOT W EQUAL 1088 OR NOT H EQUAL 1088)
  message(FATAL_ERROR
    "--dump-inter-size did not take: the fixture is ${W}x${H}.  This test is "
    "worth nothing at any other size.")
endif()

set(common --in ${YUV} --w ${W} --h ${H} --pix yuv420p --qp 26
           --frames ${FRAMES} --nsub 3 --matrix 1 --wm 0 --tskip off
           --chroma-qp-off 0 --ctx v3 --sign-hide --eyes 1 --intra-dir off
           --quiet)
set(interargs --poses ${POSES} --intra-period 6)

# ---- 1. the normative encoder, with the coded vector SEARCHED.
execute_process(COMMAND ${NXVENC} ${common} ${interargs}
                        --no-rdo --custom-tables --split4x4 off --cfl off
                        --tab v2 --xform 8 --entropy rans
                        --inter on --int-decision on
                        --int-coded-vectors static
                        --preset fast --me-effort 1 --quad-mv off
                        --near-skip off --drift-refresh off
                        --out ${WORKDIR}/ref.nxv
                RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-enc failed (${rc})")
endif()

execute_process(COMMAND ${VKENC} ${common} ${interargs} --inter
                        --coded-vectors --custom-tables --tab v2
                        --device ${DEVICE} --out ${WORKDIR}/gpu.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE eout)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc failed (${rc}): ${eout}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                        ${WORKDIR}/ref.nxv ${WORKDIR}/gpu.nxv
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "at 289 tiles the GPU coded-vector stream is not byte-identical to "
    "nxv-enc's.  It is identical at 12 tiles, so look for a field or a "
    "prefix sum that only overflows above 256.")
endif()

# ---- 2. the normative decoder.
foreach(leg ref gpu)
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/${leg}.nxv
                          --out ${WORKDIR}/${leg}.yuv --pix yuv420p --quiet
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-dec refused the ${leg} stream at 289 tiles (${rc})")
  endif()
endforeach()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                        ${WORKDIR}/ref.yuv ${WORKDIR}/gpu.yuv
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the two streams decode to different pixels at 289 tiles")
endif()

# ---- 3. the GPU decoder: its host parser is a second implementation of the
# tile-row / tile walk, and it is the one that refuses a frame outright.
if(VKDEC)
  execute_process(COMMAND ${VKDEC} --in ${WORKDIR}/gpu.nxv
                          --out ${WORKDIR}/vk.yuv --pix yuv420p --quiet
                  RESULT_VARIABLE rc ERROR_VARIABLE derr)
  if(rc EQUAL 77)
    message(STATUS "SKIP: no Vulkan device for the decoder leg")
  elseif(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "the GPU decoder refused a stream nxv-dec accepts, at 289 tiles "
      "(${rc}): ${derr}")
  else()
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                            ${WORKDIR}/gpu.yuv ${WORKDIR}/vk.yuv
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR
        "the GPU decoder and nxv-dec disagree on the coded-vector stream at "
        "289 tiles")
    endif()
  endif()
endif()

# ---- 4. the library ABI, which is what WiVRn links.  DEFAULT is STATIC.
if(VKENCAPI)
  execute_process(COMMAND ${VKENCAPI} --in ${YUV} --w ${W} --h ${H} --qp 26
                          --frames ${FRAMES} --matrix 1
                          --inter --intra-period 6 --poses ${POSES}
                          --coded-vectors default
                          --out ${WORKDIR}/api.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc-api failed at 289 tiles (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/gpu.nxv ${WORKDIR}/api.nxv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "the library ABI and the harness produce different coded-vector "
      "streams at 289 tiles")
  endif()
endif()
