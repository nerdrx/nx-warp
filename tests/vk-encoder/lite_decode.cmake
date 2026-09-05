# lite_decode.cmake -- the other half of ENTROPY_LITE: what this project's own
# GPU DECODER does with what this project's GPU encoder produced.
#
# SPDX-License-Identifier: Apache-2.0
#
# `acid.cmake -DENTROPY=lite` proves the encoder's Lite stream is byte-identical
# to `nxv-enc --entropy lite-fixed`, and `nxv-dec` decodes it.  Neither says
# anything about `nxvc_vk_decoder`, which is the decoder that actually runs on
# the headset and the whole reason the tool exists -- Pass A's rANS round chain
# is what Lite is trading bytes to avoid.
#
# So this closes the loop: encode with the GPU encoder at tool bit 30, decode
# the SAME file with `nxv-dec` and with `nxvc-vkdec`, and require the two YUVs
# to be identical.  Intra and inter, because they take different paths through
# Pass A's tile walk and because an inter Lite tile carries the optional vector
# field between the tile header and a payload whose layout the tool changed --
# the one place where the entropy tool and the tile header meet.
#
# Variables: VKENC, VKDEC, NXVDEC, WORKDIR.  Skips with a printed "SKIP:" when
# there is no usable Vulkan device, which is what the ctest matches on.

cmake_minimum_required(VERSION 3.22)

if(NOT VKENC OR NOT VKDEC OR NOT NXVDEC OR NOT WORKDIR)
  message(FATAL_ERROR "lite_decode.cmake: VKENC/VKDEC/NXVDEC/WORKDIR required")
endif()

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

execute_process(COMMAND ${VKENC} --list RESULT_VARIABLE rc OUTPUT_QUIET
                ERROR_QUIET)
if(rc EQUAL 77)
  message(STATUS "SKIP: no Vulkan ICD or no physical device")
  return()
endif()

# One description of the picture and the pose track, written by the encoder
# itself, so the intra and inter legs share a source and neither can be given
# material the other was not.
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

set(common --in ${YUV} --w ${W} --h ${H} --pix yuv420p --frames ${FRAMES}
           --nsub 3 --matrix 1 --wm 0 --tskip off --chroma-qp-off 0
           --ctx v3 --eyes 1 --intra-dir off --entropy lite --quiet)

set(nchecked 0)
foreach(leg intra inter)
  set(legargs)
  if(leg STREQUAL "inter")
    # WARP_SKIP, INTRA and STATIC_MV all in one sequence: the coded-vector
    # tiles are the ones carrying the optional field, and a tile-header
    # mistake there moves every payload byte after it.
    set(legargs --inter --coded-vectors --poses ${POSES} --intra-period 6)
  endif()
  # The quantisers the headset actually runs at, plus the ends of the range.
  foreach(qp 22 30 40)
    execute_process(COMMAND ${VKENC} ${common} --qp ${qp} ${legargs}
                            --device 0 --out ${WORKDIR}/e.nxv
                    RESULT_VARIABLE rc ERROR_VARIABLE eout)
    if(rc EQUAL 77)
      message(STATUS "SKIP: no usable Vulkan device for the encoder")
      return()
    endif()
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "${leg} QP ${qp}: nxvc-vkenc failed (${rc}): ${eout}")
    endif()

    execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/e.nxv
                            --out ${WORKDIR}/ref.yuv --pix yuv420p --quiet
                    RESULT_VARIABLE rc OUTPUT_QUIET)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR
              "${leg} QP ${qp}: nxv-dec refused the Lite stream (${rc})")
    endif()

    execute_process(COMMAND ${VKDEC} --in ${WORKDIR}/e.nxv
                            --out ${WORKDIR}/gpu.yuv --pix yuv420p --quiet
                    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE dout)
    if(rc EQUAL 77)
      message(STATUS "SKIP: no usable Vulkan device for the decoder")
      return()
    endif()
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR
              "${leg} QP ${qp}: nxvc-vkdec refused the Lite stream (${rc}): "
              "${dout}")
    endif()

    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                            ${WORKDIR}/ref.yuv ${WORKDIR}/gpu.yuv
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR
              "${leg} QP ${qp}: nxvc_vk_decoder and nxv-dec disagree on the "
              "GPU encoder's ENTROPY_LITE stream")
    endif()
    math(EXPR nchecked "${nchecked} + 1")
  endforeach()
endforeach()

file(REMOVE_RECURSE ${WORKDIR})
if(nchecked EQUAL 0)
  message(FATAL_ERROR "lite_decode.cmake: nothing ran")
endif()
message(STATUS "nxvc_vk_decoder matches nxv-dec on ${nchecked} ENTROPY_LITE "
               "streams from the GPU encoder (${FRAMES} frames each, intra "
               "and inter)")
