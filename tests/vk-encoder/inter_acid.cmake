# vk.encoder.inter.acid -- the inter pipeline against the reference, byte for
# byte, and the encoder's reference ring against the decoder's picture.
#
# The same contract the intra acid test pins, extended to Phase 2:
#
#   1. the GPU stream must be byte-identical to `nxv-enc` at the matching
#      configuration -- ADR-0028's integer mode decision, skip and intra only;
#   2. `nxv-dec` must decode both to identical pixels;
#   3. the encoder's reference ring must equal, sample for sample, the luma the
#      reference decoder produces from that same stream.
#
# (3) is the one that is new in kind.  The ring is written by the DECODER's own
# Pass B, compiled into the encoder from the decoder's source, so the property
# is meant to be structural -- but this directory has now been wrong three
# times about a structural claim (`nxe_enc.h`'s pred_src buffer, the static
# assertions that could not see a GLSL define, and E3 "already holding" a
# reconstruction it only holds in the directional build).  So it is measured,
# and a single differing sample fails.  A wrong ring is the one bug class that
# does not show up as a broken frame: it shows up as drift, seconds later.
#
# The reference flag set names every tool this pipeline does not implement,
# rather than relying on a default:
#
#   --drift-refresh off   is load-bearing and was NOT obvious.  nxv-enc
#                         defaults it ON, and the drift-driven scheme replaces
#                         `refresh_due` with an age cap and a measured-drift
#                         gate -- so the reference simply does not force the
#                         rolling refresh the GPU forces, and nine tiles in
#                         eight frames came out INTRA on one side and
#                         WARP_SKIP on the other.  The GPU implements the FIXED
#                         scheme of PAPER 2.6; the drift scheme needs an exact
#                         client shadow the encoder does not keep.
#   --int-coded-vectors  is `off` in the first leg and `static` in the second.
#                        WARP_MV is still not searched on the GPU: its
#                        predictor is the full homography, and reproducing that
#                        outside Pass W would be a second copy of the one piece
#                        of arithmetic this project refuses to have two of.
#
# The entropy tools the library now ships ON are ON here too -- frame-trained
# tables, TAB_V2 and CTX_V3 -- so this covers the inter path composed with
# them rather than a smaller stream than the library actually emits.  The
# composition is not free of consequence: a transmitted table area and
# warp_ext() both sit between the frame header and the first row header, and
# SYNTAX.md 12 orders them
#   frame := frame_header [warp_ext] [custom_matrices] [table_set]* tile_row*
# so warp_ext goes FIRST and the table area starts at 40 + warp_bytes.  Both
# encoders agreeing on the wrong order would produce a frame no decoder reads,
# which is why the decode step below is not redundant with the compare.
#
# Expects VKENC, NXVENC, NXVDEC, WORKDIR, DEVICE.

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

# ENTROPY ("rans", the default, or "lite") drives BOTH encoders' entropy tool.
# Lite also turns sign hiding and the transmitted tables off in both -- the
# syntax forbids the combination and each encoder resolves it at create() --
# so those flags travel with it rather than being spelled inline.
if(NOT DEFINED ENTROPY)
  set(ENTROPY rans)
endif()
if(ENTROPY STREQUAL "lite")
  set(REF_ENT --entropy lite-fixed --no-sign-hide --no-custom-tables --tab v1)
  set(GPU_ENT --entropy lite)
  set(API_ENT --entropy lite)
else()
  set(REF_ENT --entropy rans --sign-hide --custom-tables --tab v2)
  set(GPU_ENT --custom-tables --tab v2)
  set(API_ENT)
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

# One description of the picture and the pose track, written by the encoder
# itself so the two encoders cannot be given different material.
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

set(common --in ${YUV} --w ${W} --h ${H} --pix yuv420p --qp 26
           --frames ${FRAMES} --nsub 3 --matrix 1 --wm 0 --tskip off
           --chroma-qp-off 0 --ctx v3 --eyes 1 --intra-dir off
           --quiet)
set(interargs --poses ${POSES} --intra-period 6)

execute_process(COMMAND ${NXVENC} ${common} ${interargs}
                        --no-rdo --split4x4 off --cfl off
                        --xform 8 ${REF_ENT}
                        --inter on --int-decision on --int-coded-vectors off
                        --preset fast --me-effort 1 --quad-mv off
                        --near-skip off --drift-refresh off
                        --out ${WORKDIR}/ref.nxv
                RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-enc failed (${rc})")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E env
                        NXE_DUMP_RING=${WORKDIR}/ring
                        ${VKENC} ${common} ${interargs} --inter
                        ${GPU_ENT}
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
    "the inter stream is not byte-identical to nxv-enc.  The reference "
    "configuration is in this file; --drift-refresh off and "
    "--int-coded-vectors off are both load-bearing.")
endif()

execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/ref.nxv
                        --out ${WORKDIR}/ref.yuv --pix yuv420p --quiet
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-dec refused the reference stream (${rc})")
endif()
execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/gpu.nxv
                        --out ${WORKDIR}/gpu.yuv --pix yuv420p --quiet
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-dec refused the GPU stream (${rc})")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                        ${WORKDIR}/ref.yuv ${WORKDIR}/gpu.yuv
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the two streams decode to different pixels")
endif()

execute_process(COMMAND ${VKENC} --w ${W} --h ${H}
                        --check-ring ${WORKDIR}/ring
                        --check-ring-decoded ${WORKDIR}/gpu.yuv
                        --check-ring-frames ${FRAMES}
                RESULT_VARIABLE rc OUTPUT_VARIABLE rout ERROR_VARIABLE rerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the encoder's reference ring is not the decoder's "
                      "picture:\n${rerr}")
endif()

# ---- the same stream through the library ABI.
#
# The harness and the library are two front ends on one pipeline, so a stream
# that differs between them is a bug in whichever one the WiVRn backend is not
# using -- which is the one nobody would notice.  The ABI leg also exercises
# the two calls a compositor makes per frame that the harness fakes from a
# file: set_view(), and the receipt map.
if(VKENCAPI)
  # `coded_vectors = NONE` must reproduce the skip-only stream, and the
  # DEFAULT must reproduce the STATIC_MV one -- which is what pins that the
  # default is STATIC rather than merely documented as such.  Both legs also
  # check the ABI against the harness: two front ends on one pipeline, and a
  # divergence would be in whichever one WiVRn is using.
  execute_process(COMMAND ${VKENCAPI} --in ${YUV} --w ${W} --h ${H} --qp 26
                          --frames ${FRAMES} --matrix 1
                          --inter --intra-period 6 --poses ${POSES}
                          ${API_ENT}
                          --coded-vectors none
                          --out ${WORKDIR}/api.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc-api failed (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/gpu.nxv ${WORKDIR}/api.nxv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "the library ABI at coded_vectors=NONE and the harness produce "
      "different inter streams")
  endif()


  # An all-zero receipt map is a full reset: the frame after it must be
  # entirely INTRA, because the client holds nothing to predict from.  The
  # harness checks it against the ABI's own tile records and fails itself.
  execute_process(COMMAND ${VKENCAPI} --in ${YUV} --w ${W} --h ${H} --qp 26
                          --frames ${FRAMES} --matrix 1
                          --inter --intra-period 6 --poses ${POSES}
                          ${API_ENT}
                          --drop-at 2
                          --out ${WORKDIR}/reset.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the receipt-map reset was not honoured: ${eout}")
  endif()
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/reset.nxv
                          --out ${WORKDIR}/reset.yuv --pix yuv420p --quiet
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-dec refused the stream a reset produced (${rc})")
  endif()
endif()

# ---- the same clip with STATIC_MV searched.
#
# A separate leg rather than a replacement: skip-only and skip-plus-vector are
# different decisions and both have to hold, and the skip-only one is what the
# library ships until the ABI exposes the other.
execute_process(COMMAND ${NXVENC} ${common} ${interargs}
                        --no-rdo --split4x4 off --cfl off
                        --xform 8 ${REF_ENT}
                        --inter on --int-decision on --int-coded-vectors static
                        --preset fast --me-effort 1 --quad-mv off
                        --near-skip off --drift-refresh off
                        --out ${WORKDIR}/ref-mv.nxv
                RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-enc failed with STATIC_MV (${rc})")
endif()
execute_process(COMMAND ${VKENC} ${common} ${interargs} --inter --coded-vectors
                        ${GPU_ENT}
                        --device ${DEVICE} --out ${WORKDIR}/gpu-mv.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE eout)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc failed with STATIC_MV (${rc}): ${eout}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                        ${WORKDIR}/ref-mv.nxv ${WORKDIR}/gpu-mv.nxv
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "the STATIC_MV stream is not byte-identical to nxv-enc.  The search runs "
    "three stages against ONE running best -- the reference never resets it "
    "between them, and the stages sample at different densities -- so a "
    "per-stage reset makes the integer refine live and moves the vectors.")
endif()
execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/gpu-mv.nxv
                        --out ${WORKDIR}/gpu-mv.yuv --pix yuv420p --quiet
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-dec refused the STATIC_MV stream (${rc})")
endif()

# The ABI's DEFAULT coded_vectors must be STATIC, which is checked here
# rather than above because it compares against the STATIC_MV stream the
# leg above produces.
if(VKENCAPI)
  execute_process(COMMAND ${VKENCAPI} --in ${YUV} --w ${W} --h ${H} --qp 26
                          --frames ${FRAMES} --matrix 1
                          --inter --intra-period 6 --poses ${POSES}
                          ${API_ENT}
                          --out ${WORKDIR}/api-mv.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc-api failed at the default (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/gpu-mv.nxv ${WORKDIR}/api-mv.nxv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "the library ABI's DEFAULT coded_vectors is not STATIC: its stream "
      "differs from the harness run with --coded-vectors")
  endif()
endif()

# ---- the reference walk, through the library ABI.
#
# The 289-tile leg (inter_1088.cmake) proves this at the headset's size; this
# is the same claim at twelve tiles, which is the size CI can afford, and it
# goes through the ABI because the ABI is what a compositor calls.
#
# `--hold-every 2` is a client that reconstructs every other frame and reports
# the rest not held; `--decode-every 2` is that same client at decode time,
# skipping the frames it never asked for so its reference ring has the holes
# the reports described.  The control is the same clip with no reports, which
# that client must REFUSE -- otherwise this test would pass against an encoder
# that ignored the reports entirely.
if(VKENCAPI)
  execute_process(COMMAND ${VKENCAPI} --in ${YUV} --w ${W} --h ${H} --qp 26
                          --frames ${FRAMES} --matrix 1
                          --inter --intra-period 180 --poses ${POSES}
                          --hold-every 2
                          --out ${WORKDIR}/hold2.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc-api --hold-every 2 failed (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${VKENCAPI} --in ${YUV} --w ${W} --h ${H} --qp 26
                          --frames ${FRAMES} --matrix 1
                          --inter --intra-period 180 --poses ${POSES}
                          --out ${WORKDIR}/nohold.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc-api (no reports) failed (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/nohold.nxv
                          --out ${WORKDIR}/nohold2.yuv --pix yuv420p
                          --decode-every 2 --quiet
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(rc EQUAL 0)
    message(FATAL_ERROR
      "a client decoding every other frame accepted a stream coded with no "
      "held reports, so this test is no longer measuring anything")
  endif()
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/hold2.nxv
                          --out ${WORKDIR}/hold2.yuv --pix yuv420p
                          --decode-every 2 --quiet
                  RESULT_VARIABLE rc ERROR_VARIABLE derr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "nxv-dec refused a frame of the --hold-every 2 stream: ${derr}")
  endif()
endif()

message(STATUS "vk.encoder.inter.acid: ${FRAMES} frames byte-identical, "
               "decoded identical, ring == decoder, ABI agrees, "
               "STATIC_MV byte-identical")
