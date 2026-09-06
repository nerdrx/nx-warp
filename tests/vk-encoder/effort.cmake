# effort.cmake -- the encoder's effort levels, against the reference encoder.
#
# SPDX-License-Identifier: Apache-2.0
#
# An effort level is only worth having if it is still the SAME BITSTREAM: it
# changes which levels are coded and never how they are decoded, so a stream
# produced at any level must be a stream `nxv-dec` reads, and it must be the
# stream `nxv-enc` produces at the matching flags.  Effort 1 is the integer
# requantiser -- `nxvc_config::int_rdoq`, `nxv-enc --int-rdoq 1` -- and this
# pins all of that:
#
#   1. the GPU pipeline at `--int-rdoq 1` is byte-identical to `nxv-enc
#      --int-rdoq 1` at the acid flag set, intra and inter, rANS and Lite;
#   2. the CPU model of E3 reaches the same stream, which is what says the
#      shader's requantiser and the model's are one rule and not two;
#   3. `nxv-dec` decodes it, and to the same pixels as the reference stream;
#   4. the LIBRARY at `create_info::effort = 1` produces that same stream, and
#      REFUSES `effort = 2`.
#
# (4)'s second half is the part that would otherwise rot.  There is no level 2
# -- a wider motion search measures -0.05 % BD-rate on the stereo clip for
# +12 % encoder time (vk/encoder/README.md, "The effort levels, measured") --
# and a create() that silently clamped an unsupported level would hand a caller
# the stream it was trying to beat, with nothing to say so.
#
# The last leg pins the WIDER SEARCH itself, at `--mv-range 31`, even though no
# effort level selects it.  It is what makes the measurement above meaningful:
# the two encoders have to be searching the same candidate set before "it does
# not pay" is a statement about the tool rather than about a disagreement.
#
# Expects VKENC, NXVENC, NXVDEC, WORKDIR, DEVICE; optionally VKENCAPI and
# VKDEC (the GPU decoder, which reads the effort-1 streams too).

cmake_minimum_required(VERSION 3.22)

if(NOT VKENC OR NOT NXVENC OR NOT NXVDEC OR NOT WORKDIR)
  message(FATAL_ERROR "effort.cmake: VKENC/NXVENC/NXVDEC/WORKDIR are required")
endif()

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

execute_process(COMMAND ${VKENC} --list RESULT_VARIABLE rc OUTPUT_QUIET
                ERROR_QUIET)
if(rc EQUAL 77 AND NOT DEVICE STREQUAL "cpu")
  message(STATUS "SKIP: no Vulkan ICD or no physical device")
  return()
endif()

set(DEVARGS --device ${DEVICE})
if(DEVICE STREQUAL "cpu")
  set(DEVARGS --cpu)
endif()

# One description of the picture and the pose track, written by the encoder
# itself, so the two encoders cannot be given different material.
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
           --chroma-qp-off 0 --ctx v3 --eyes 1 --intra-dir off --quiet)
set(minor6_off --no-rdo --split4x4 off --cfl off --xform 8)
set(interref --poses ${POSES} --intra-period 6
             --inter on --int-decision on --int-coded-vectors static
             --preset fast --me-effort 1 --quad-mv off --near-skip off
             --drift-refresh off)
set(intergpu --poses ${POSES} --intra-period 6 --inter --coded-vectors)

# name  refent  gpuent  refmode  gpumode  extra-ref  extra-gpu
function(leg name refent gpuent refmode gpumode refextra gpuextra)
  execute_process(COMMAND ${NXVENC} ${common} ${minor6_off} ${refent}
                          ${refmode} ${refextra} --out ${WORKDIR}/${name}.ref
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: nxv-enc failed (${rc})")
  endif()
  execute_process(COMMAND ${VKENC} ${common} ${gpuent} ${gpumode} ${gpuextra}
                          ${DEVARGS} --out ${WORKDIR}/${name}.gpu
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(rc EQUAL 77)
    message(STATUS "SKIP: no usable Vulkan device for this configuration")
    return()
  endif()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: nxvc-vkenc failed (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/${name}.ref ${WORKDIR}/${name}.gpu
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "${name}: the stream is not byte-identical to nxv-enc at the matching "
      "flags.  An effort level that is not byte-identical is not an effort "
      "level; it is a different encoder.")
  endif()
  # It has to decode, and to the reference stream's own pixels: two streams
  # can agree byte for byte and both be a frame nxv-dec refuses.
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/${name}.ref
                          --out ${WORKDIR}/${name}.ref.yuv --pix yuv420p
                          --quiet RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: nxv-dec refused the reference stream (${rc})")
  endif()
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/${name}.gpu
                          --out ${WORKDIR}/${name}.gpu.yuv --pix yuv420p
                          --quiet RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: nxv-dec refused the GPU stream (${rc})")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/${name}.ref.yuv ${WORKDIR}/${name}.gpu.yuv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: the two streams decode to different pixels")
  endif()
endfunction()

set(RANS_REF --entropy rans --sign-hide --custom-tables --tab v2)
set(RANS_GPU --custom-tables --tab v2)
set(LITE_REF --entropy lite-fixed --no-sign-hide --no-custom-tables --tab v1)
set(LITE_GPU --entropy lite)

# ---- effort 1, intra and inter, over both entropy tools.
#
# Lite is not a formality here.  The requantiser runs BEFORE sign hiding, and
# Lite has no sign hiding at all, so the two tools exercise the two orders in
# which the level and the hidden parity can meet.
leg(e1_intra_rans "${RANS_REF}" "${RANS_GPU}" "" "" "--int-rdoq;1" "--int-rdoq;1")
leg(e1_intra_lite "${LITE_REF}" "${LITE_GPU}" "" "" "--int-rdoq;1" "--int-rdoq;1")
if(NOT DEVICE STREQUAL "cpu")
  # The inter path has no CPU model, so these two are GPU-only.
  leg(e1_inter_rans "${RANS_REF}" "${RANS_GPU}" "${interref}" "${intergpu}"
      "--int-rdoq;1" "--int-rdoq;1")
  leg(e1_inter_lite "${LITE_REF}" "${LITE_GPU}" "${interref}" "${intergpu}"
      "--int-rdoq;1" "--int-rdoq;1")

  # ---- the wider search, which no effort level selects.  See the header.
  leg(mv31 "${RANS_REF}" "${RANS_GPU}" "${interref}" "${intergpu}"
      "--mv-range;31" "--mv-range;31")
  leg(mv31_rdoq "${RANS_REF}" "${RANS_GPU}" "${interref}" "${intergpu}"
      "--mv-range;31;--int-rdoq;1" "--mv-range;31;--int-rdoq;1")
endif()

# ---- effort 0 is unchanged.  The level is opt-in or it is a silent bitstream
# change for every caller that never asked for one.
leg(e0_intra_rans "${RANS_REF}" "${RANS_GPU}" "" "" "" "")

# ---- the GPU decoder, on an effort-1 stream.
#
# A level that only `nxv-dec` reads is not shipped.  The requantiser drops
# levels, and a dropped level is a shorter coding unit, a different `last`
# position and a different rANS round count -- all things the headset's Pass A
# walks itself.  So the stream goes through the decoder that runs on the
# headset as well as through the normative one, and the two pictures must be
# the same.
if(VKDEC AND NOT DEVICE STREQUAL "cpu")
  foreach(leg e1_intra_rans e1_inter_rans e1_intra_lite e1_inter_lite)
    if(EXISTS ${WORKDIR}/${leg}.gpu)
      execute_process(COMMAND ${VKDEC} --in ${WORKDIR}/${leg}.gpu
                              --out ${WORKDIR}/${leg}.vk.yuv --pix yuv420p
                              --quiet
                      RESULT_VARIABLE rc ERROR_VARIABLE derr)
      if(rc EQUAL 77)
        message(STATUS "SKIP: no Vulkan device for the decoder leg")
      elseif(NOT rc EQUAL 0)
        message(FATAL_ERROR
          "${leg}: the GPU decoder refused a stream nxv-dec accepts (${rc}): "
          "${derr}")
      else()
        execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                                ${WORKDIR}/${leg}.gpu.yuv
                                ${WORKDIR}/${leg}.vk.yuv
                        RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
          message(FATAL_ERROR
            "${leg}: the GPU decoder and nxv-dec disagree on an effort-1 "
            "stream")
        endif()
      endif()
    endif()
  endforeach()
endif()

# ---- the library ABI: effort 1 is the same stream, effort 2 is refused.
if(VKENCAPI AND NOT DEVICE STREQUAL "cpu")
  execute_process(COMMAND ${VKENCAPI} --in ${YUV} --w ${W} --h ${H} --qp 26
                          --frames ${FRAMES} --matrix 1 --effort 1
                          --out ${WORKDIR}/api1.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc-api --effort 1 failed (${rc}): ${eout}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/e1_intra_rans.gpu ${WORKDIR}/api1.nxv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "the library at effort 1 and the harness at --int-rdoq 1 produce "
      "different streams: the ABI's mapping of the level is wrong")
  endif()

  execute_process(COMMAND ${VKENCAPI} --in ${YUV} --w ${W} --h ${H} --qp 26
                          --frames 1 --matrix 1 --effort 2
                          --out ${WORKDIR}/api2.nxv
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(rc EQUAL 0)
    message(FATAL_ERROR
      "the library ACCEPTED effort = 2.  There is no level 2: a wider search "
      "does not pay (vk/encoder/README.md), and a level that is silently "
      "clamped hands the caller the stream it was trying to beat.")
  endif()
endif()

message(STATUS "effort: byte-identical to nxv-enc at every level, ${W}x${H}, "
               "${FRAMES} frames, device ${DEVICE}")
