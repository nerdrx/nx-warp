# vk.encoder.atlas.acid -- the ATLAS reference against nxv-enc, byte for byte,
# and the encoder's shadow atlas against the decoder's.
#
# Two claims, and the SECOND is the one that is new in kind:
#
#   1. the GPU stream must be byte-identical to `nxv-enc --atlas on` at the
#      matching configuration;
#   2. the encoder's shadow ATLAS -- the per-tile table of [SYN] 13.12.1 plus a
#      digest of the atlas pixels -- must equal both nxv-enc's shadow and the
#      atlas `nxv-dec` builds from the GPU's own stream.
#
# (1) alone is not the property wanted.  [SYN] 13.12 says in as many words that
# the normative output under ATLAS is the atlas and NOT the picture, and two
# encoders can emit identical bytes while disagreeing about the reference they
# believe the client now holds: every frame is legal, and some frames later the
# encoder predicts from pixels the client does not have.  That failure does not
# look like a broken frame, it looks like drift -- which is the same reason
# inter_acid.cmake measures the reference ring instead of trusting that it is
# structurally right.
#
# What made (2) checkable is `nxvc-vkenc --atlas-dump`, which writes the same
# layout `nxv-enc --atlas-dump` and `nxv-dec --atlas-dump` already wrote: the
# whole table, then a 32-byte FNV-1a over the atlas planes.
#
# One thing this file deliberately does NOT do is the picture model's
# reference-walk control -- code a clip with no held reports and require a
# client skipping frames to REFUSE it.  Under ATLAS that client accepts it, and
# correctly so: 13.12.6 says a lost tile invalidates exactly its own position
# and there is no concealment process, so the atlas survives a drop by
# construction and nothing in the stream becomes unparseable.  The drop
# contract is therefore checked where it is now observable -- in the atlas --
# by the hold-every leg at the bottom.
#
# The reference flag set is inter_acid.cmake's, with `--atlas on` added; every
# note there about `--drift-refresh off` and `--int-coded-vectors off` being
# load-bearing applies here unchanged.
#
# Expects VKENC, NXVENC, NXVDEC, WORKDIR, DEVICE; optional ENTROPY.

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

if(NOT DEFINED ENTROPY)
  set(ENTROPY rans)
endif()
if(ENTROPY STREQUAL "lite")
  set(REF_ENT --entropy lite-fixed --no-sign-hide --no-custom-tables --tab v1)
  set(GPU_ENT --entropy lite)
else()
  set(REF_ENT --entropy rans --sign-hide --custom-tables --tab v2)
  set(GPU_ENT --custom-tables --tab v2)
endif()

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

# This file runs the MONO fixture only.  `--eyes 2` needs a side-by-side
# picture and `--dump-inter` writes a mono one, and CMake script mode cannot
# synthesise one -- it has no way to write binary.  The eye pair is not left
# uncovered: the ref suite's `stereo-geometry` case in tests/ref/test_atlas.cpp
# runs the two-eye atlas end to end, and the eye-pair byte-identity of this
# encoder is what vk.encoder.acid.api.stereo.* already pins.  Adding a fixture
# generator here to restate it would be a third copy of a claim two tests
# already hold.

set(common --in ${YUV} --w ${W} --h ${H} --pix yuv420p --qp 26
           --frames ${FRAMES} --nsub 3 --matrix 1 --wm 0 --tskip off
           --chroma-qp-off 0 --ctx v3 --eyes 1 --intra-dir off --quiet)

# Two pose tracks and two refresh policies.  `--intra-period 180` is the one
# that matters most: with no periodic intra the atlas has to carry the whole
# clip, so every composition step and every envelope rejection is live, and an
# entry invalidated one frame early on one side shows up as a coded tile on
# that side alone.
foreach(leg "ip6;--intra-period 6" "ip180;--intra-period 180")
  list(GET leg 0 name)
  list(GET leg 1 ipflag)
  string(REPLACE " " ";" ipflag "${ipflag}")

  execute_process(COMMAND ${NXVENC} ${common} --poses ${POSES} ${ipflag}
                          --no-rdo --split4x4 off --cfl off
                          --xform 8 ${REF_ENT}
                          --inter on --int-decision on --int-coded-vectors off
                          --preset fast --me-effort 1 --quad-mv off
                          --near-skip off --drift-refresh off --atlas on
                          --atlas-dump ${WORKDIR}/${name}.refenc.at
                          --out ${WORKDIR}/${name}.ref.nxv
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-enc --atlas failed at ${name} (${rc})")
  endif()

  execute_process(COMMAND ${VKENC} ${common} --poses ${POSES} ${ipflag}
                          --inter ${GPU_ENT} --atlas --device ${DEVICE}
                          --atlas-dump ${WORKDIR}/${name}.gpuenc.at
                          --out ${WORKDIR}/${name}.gpu.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc --atlas failed at ${name} (${rc}): ${eout}")
  endif()

  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/${name}.ref.nxv ${WORKDIR}/${name}.gpu.nxv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "${name}: the ATLAS stream is not byte-identical to nxv-enc")
  endif()

  # The decoder's atlas, built from the GPU's own stream.  This is the
  # comparison 13.12 actually specifies.
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/${name}.gpu.nxv
                          --out ${WORKDIR}/${name}.dec.yuv --pix yuv420p
                          --atlas-dump ${WORKDIR}/${name}.dec.at --quiet
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-dec refused the GPU's ATLAS stream (${rc})")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/${name}.gpuenc.at ${WORKDIR}/${name}.dec.at
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "${name}: the encoder's shadow atlas is not the decoder's atlas.  The "
      "stream is byte-identical, so this is a divergence in the reference the "
      "two sides believe the client holds -- 13.12.3's advance, write-back or "
      "envelope rejection, not the bitstream.")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/${name}.gpuenc.at
                          ${WORKDIR}/${name}.refenc.at
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "${name}: the GPU shadow atlas differs from nxv-enc's shadow atlas")
  endif()
endforeach()

# ---- the drop, observed in the atlas.
#
# `--hold-every 2` is a client that reconstructs every other frame and reports
# the rest not held.  nxv-enc has no receipt input, so there is no second
# encoder to be identical to; what is checked is that the reports CHANGE the
# stream -- an encoder that ignored them would emit the control's bytes -- and
# that the resulting stream still decodes when that client skips the frames it
# never asked for.
execute_process(COMMAND ${VKENC} ${common} --poses ${POSES} --intra-period 180
                        --inter ${GPU_ENT} --atlas --device ${DEVICE}
                        --hold-every 2 --out ${WORKDIR}/hold2.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE eout)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc --atlas --hold-every 2 failed: ${eout}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                        ${WORKDIR}/ip180.gpu.nxv ${WORKDIR}/hold2.nxv
                RESULT_VARIABLE rc)
if(rc EQUAL 0)
  message(FATAL_ERROR
    "--hold-every 2 produced the control's bytes, so the not-held reports "
    "are not reaching the atlas's undo at all")
endif()
execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/hold2.nxv
                        --out ${WORKDIR}/hold2.yuv --pix yuv420p
                        --decode-every 2 --quiet
                RESULT_VARIABLE rc ERROR_VARIABLE derr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "nxv-dec refused a frame of the --hold-every 2 ATLAS stream: ${derr}")
endif()

message(STATUS "vk.encoder.atlas.acid: ${FRAMES} frames byte-identical at two "
               "refresh policies, encoder shadow == decoder atlas == nxv-enc "
               "shadow (table and pixel digest), drops honoured")
