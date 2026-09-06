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
                        --dump-inter-size 1088 1088 16
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

# Legs 1-4 code the first eight frames; the drop legs at the end want a
# longer run, and the fixture's content at frame n does not depend on how many
# frames were dumped, so one fixture serves both.
set(SHORT 8)
set(common --in ${YUV} --w ${W} --h ${H} --pix yuv420p --qp 26
           --frames ${SHORT} --nsub 3 --matrix 1 --wm 0 --tskip off
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
                          --frames ${SHORT} --matrix 1
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

# ---- 5. `ref_sel`, against the normative encoder at each of its three legal
# values.
#
# `ref_sel` is two bits of every inter tile header, the slot the warp matrix
# is derived from, and the slot E1c's search reads.  Getting any one of those
# three wrong produces a stream that still decodes -- from the wrong picture --
# so the check has to be byte-identity against `nxv-enc --ref-sel d`, not a
# decode.  Distance 0 is the value every other test in the tree exercises;
# 1 and 2 have never been emitted by this encoder before.
foreach(d 0 1 2)
  execute_process(COMMAND ${NXVENC} ${common} ${interargs}
                          --no-rdo --custom-tables --split4x4 off --cfl off
                          --tab v2 --xform 8 --entropy rans
                          --inter on --int-decision on
                          --int-coded-vectors static
                          --preset fast --me-effort 1 --quad-mv off
                          --near-skip off --drift-refresh off
                          --ref-sel ${d}
                          --out ${WORKDIR}/ref-d${d}.nxv
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-enc --ref-sel ${d} failed (${rc})")
  endif()
  execute_process(COMMAND ${VKENC} ${common} ${interargs} --inter
                          --coded-vectors --custom-tables --tab v2
                          --ref-sel ${d}
                          --device ${DEVICE} --out ${WORKDIR}/gpu-d${d}.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc --ref-sel ${d} failed (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/ref-d${d}.nxv ${WORKDIR}/gpu-d${d}.nxv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "at ref_sel ${d} the GPU stream is not byte-identical to nxv-enc's at "
      "289 tiles.  The three places that carry the choice are word1 bits "
      "21-22, the warp_ext() the reference slot's view derives, and the ring "
      "slot E1c searches.")
  endif()
endforeach()

# ---- 6. the drop storm, which is what all of this is for.
#
# A headset that cannot decode every frame drops the ones it cannot reach and
# says so.  Before nxvc_vk_encoder_set_frame_held() the only answer was an
# all-INTRA resync; now the encoder walks `ref_sel` out to the newest frame the
# client still holds, and the frames the client DOES decode must decode.
#
# `--hold-every N` on the encoder is that client's report; `--decode-every N`
# on the two decoders is that client -- it skips the frames it did not ask for
# without parsing them, so its reference ring has exactly the holes the report
# described.  The control leg is the same clip coded with NO report, which
# must be REFUSED: without it this test would pass against an encoder that
# ignored the whole mechanism.
set(dropcommon --in ${YUV} --w ${W} --h ${H} --pix yuv420p --qp 26
               --frames ${FRAMES} --nsub 3 --matrix 1 --ctx v3 --intra-dir off
               --quiet --poses ${POSES} --intra-period 180 --inter
               --coded-vectors --custom-tables --tab v2 --device ${DEVICE})

execute_process(COMMAND ${VKENC} ${dropcommon} --out ${WORKDIR}/nohold.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE eout)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc (no reports) failed (${rc}): ${eout}")
endif()

foreach(n 2 4)
  execute_process(COMMAND ${VKENC} ${dropcommon} --hold-every ${n}
                          --out ${WORKDIR}/hold${n}.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc --hold-every ${n} failed (${rc}): ${eout}")
  endif()

  # The control: the un-reported stream must be refused by this same client.
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/nohold.nxv
                          --out ${WORKDIR}/nohold-${n}.yuv --pix yuv420p
                          --decode-every ${n} --quiet
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(rc EQUAL 0)
    message(FATAL_ERROR
      "a client decoding one frame in ${n} accepted a stream coded with no "
      "held reports.  That is supposed to be the failure this test proves is "
      "fixed, so the test is no longer measuring anything.")
  endif()

  # And the reported stream must be accepted, by both decoders, identically.
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/hold${n}.nxv
                          --out ${WORKDIR}/hold${n}.yuv --pix yuv420p
                          --decode-every ${n} --quiet
                  RESULT_VARIABLE rc ERROR_VARIABLE derr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "nxv-dec refused a frame of the --hold-every ${n} stream: ${derr}")
  endif()
  if(VKDEC)
    execute_process(COMMAND ${VKDEC} --in ${WORKDIR}/hold${n}.nxv
                            --out ${WORKDIR}/hold${n}-vk.yuv --pix yuv420p
                            --decode-every ${n} --quiet
                    RESULT_VARIABLE rc ERROR_VARIABLE derr)
    if(rc EQUAL 77)
      message(STATUS "SKIP: no Vulkan device for the drop-storm decoder leg")
    elseif(NOT rc EQUAL 0)
      message(FATAL_ERROR
        "the GPU decoder refused a frame of the --hold-every ${n} stream at "
        "289 tiles: ${derr}")
    else()
      execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                              ${WORKDIR}/hold${n}.yuv
                              ${WORKDIR}/hold${n}-vk.yuv
                      RESULT_VARIABLE rc)
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR
          "the two decoders disagree on the --hold-every ${n} stream")
      endif()
    endif()
  endif()
endforeach()

# ---- 7. the same walk under ENTROPY_LITE.
#
# A Lite tile carries the same word1 as a rANS one -- the entropy tool reaches
# the payload, not the tile header -- but it is written by a DIFFERENT kernel
# (lite_encode.comp, not rans_encode.comp), so "the same" is a claim about two
# pieces of source and has to be measured.  A missing ref_sel there would be
# invisible on every rANS test in the tree and would black-screen the Pico,
# which is the device Lite exists for.
execute_process(COMMAND ${VKENC} ${dropcommon} --entropy lite --hold-every 2
                        --out ${WORKDIR}/lite2.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE eout)
if(rc EQUAL 0)
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/lite2.nxv
                          --out ${WORKDIR}/lite2.yuv --pix yuv420p
                          --decode-every 2 --quiet
                  RESULT_VARIABLE rc ERROR_VARIABLE derr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "nxv-dec refused a frame of the Lite --hold-every 2 stream: ${derr}.  "
      "lite_encode.comp has to write word1 bits 21-22 exactly as "
      "rans_encode.comp does.")
  endif()
  if(VKDEC)
    execute_process(COMMAND ${VKDEC} --in ${WORKDIR}/lite2.nxv
                            --out ${WORKDIR}/lite2-vk.yuv --pix yuv420p
                            --decode-every 2 --quiet
                    RESULT_VARIABLE rc ERROR_VARIABLE derr)
    if(NOT rc EQUAL 0 AND NOT rc EQUAL 77)
      message(FATAL_ERROR
        "the GPU decoder refused a frame of the Lite --hold-every 2 stream: "
        "${derr}")
    endif()
    if(rc EQUAL 0)
      execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                              ${WORKDIR}/lite2.yuv ${WORKDIR}/lite2-vk.yuv
                      RESULT_VARIABLE rc)
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR
          "the two decoders disagree on the Lite --hold-every 2 stream")
      endif()
    endif()
  endif()
else()
  message(STATUS "vk.encoder.inter.cv1088: --entropy lite not available, "
                 "Lite leg skipped")
endif()
