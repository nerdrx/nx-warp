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
#   --int-coded-vectors off  STATIC_MV and WARP_MV are the next increment.
#
# Expects VKENC, NXVENC, NXVDEC, WORKDIR, DEVICE.

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
           --chroma-qp-off 0 --ctx v2 --eyes 1 --intra-dir off --quiet)
set(interargs --poses ${POSES} --intra-period 6)

execute_process(COMMAND ${NXVENC} ${common} ${interargs}
                        --no-rdo --no-custom-tables --split4x4 off --cfl off
                        --tab v1 --xform 8 --entropy rans
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

message(STATUS "vk.encoder.inter.acid: ${FRAMES} frames byte-identical, "
               "decoded identical, ring == decoder")
