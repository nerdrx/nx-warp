# api_acid.cmake -- the acid test for the LIBRARY rather than for the harness.
#
# SPDX-License-Identifier: Apache-2.0
#
# acid.cmake drives nxvc-vkenc, which reaches the kernels through nxe::Config
# directly.  It therefore proves the kernels and says nothing about
# nxvc_vk_encoder's own configuration mapping: which tools the ABI turns off,
# which quantiser matrix it picks, what it puts in the stream header.  That
# mapping is the thing an integrator actually links, and it is code acid.cmake
# never executes.
#
# So this runs the same comparison one level up.  The ABI fixes every coding
# decision except the picture, the QP and the quantiser matrix, so there is one
# configuration to check rather than a table -- and it is the configuration
# WiVRn's "backend": "vk" runs.  The reference is driven with every bitstream
# minor-6 tool named and turned off, exactly as acid.cmake names them, because
# four of the six are ON in nxvc_config_default().
#
# IMAGE=ON runs the same comparison through the OTHER entry point:
# nxvc_vk_encoder_encode_image(), with the picture in a two-plane 4:2:0 VkImage
# the tool builds the way a Linux compositor does, so E0 reads it on the device
# and no plane is ever laid out on the host.  That path produces the bitstream
# or it does not, and "byte-identical to nxv-enc" is the only useful way to
# say which -- a picture read one pixel off still decodes to something.
#
# ENTROPY=lite runs the same comparison with the ABI's `entropy` field set to
# NXVC_VKE_ENTROPY_LITE, which is what a server picks when the client's tool
# mask advertises bit 30.  It is the same claim about the same mapping: the
# three tools Lite is incompatible with have to come OUT of the stream header
# as well as out of the coding, and only a byte comparison says so.
#
# Required: APIENC, NXVENC, NXVDEC, WORKDIR.  Skips (via "SKIP:" in the output,
# which the ctest carries a SKIP_REGULAR_EXPRESSION for) when there is no
# usable Vulkan device.

cmake_minimum_required(VERSION 3.22)

if(NOT APIENC OR NOT NXVENC OR NOT NXVDEC OR NOT WORKDIR)
  message(FATAL_ERROR "api_acid.cmake: APIENC/NXVENC/NXVDEC/WORKDIR are required")
endif()

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

# A picture with structure the codec has to work for.  `nxv-enc --gen` is not a
# thing, so the source is synthesized here with CMake's own file(WRITE) being
# far too slow -- instead the reference encoder's own test pattern is reused by
# asking nxvc-vkenc for it.  When that tool is absent this test does not exist
# (the CMakeLists guards on it), so VKENC is always set when we get here.
if(NOT VKENC)
  message(FATAL_ERROR "api_acid.cmake: VKENC is required")
endif()
execute_process(COMMAND ${VKENC} --dump-selftest-yuv ${WORKDIR}/
                OUTPUT_VARIABLE cases RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc --dump-selftest-yuv failed: ${rc}")
endif()

# Take the first single-eye 4:2:0 case as the picture; the ABI is single-eye
# 4:2:0 only, so any other row is one it could not encode anyway.
string(REPLACE "\n" ";" lines "${cases}")
set(picked "")
foreach(line ${lines})
  if(line STREQUAL "")
    continue()
  endif()
  string(REPLACE " " ";" f "${line}")
  list(GET f 1 w)
  list(GET f 2 h)
  list(GET f 3 eyes)
  list(GET f 4 pix)
  if(eyes STREQUAL "1" AND pix STREQUAL "yuv420p")
    list(GET f 0 picked)
    set(pw ${w})
    set(ph ${h})
    break()
  endif()
endforeach()
if(picked STREQUAL "")
  message(FATAL_ERROR "api_acid.cmake: no single-eye 4:2:0 case to test with")
endif()

set(APIARGS)
set(WHAT "nxvc_vk_encoder")
if(IMAGE)
  set(APIARGS --image)
  set(WHAT "nxvc_vk_encoder's image entry point")
endif()
# The entropy tool, and the three tools it turns off.  A Lite stream carries
# NEITHER sign hiding NOR custom tables NOR TAB_V2, and the reference makes the
# same substitution at create(), so the flags below are named on both sides and
# the two encoders resolve them identically or the comparison fails.
set(REF_ENTROPY --entropy rans --sign-hide --custom-tables --tab v2)
if(ENTROPY STREQUAL "lite")
  set(APIARGS ${APIARGS} --entropy lite)
  set(REF_ENTROPY --entropy lite-fixed --no-sign-hide --no-custom-tables
                  --tab v1)
  set(WHAT "${WHAT} at ENTROPY_LITE")
endif()

set(nchecked 0)
foreach(qp 20 26 30 40)
  # The library.  Exit 77 is "no usable Vulkan device", which is a skip.
  execute_process(COMMAND ${APIENC} --in ${picked} --w ${pw} --h ${ph}
                          --qp ${qp} --frames 3 --matrix 1 ${APIARGS}
                          --out ${WORKDIR}/api.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(rc EQUAL 77)
    message(STATUS "SKIP: no usable Vulkan device for the encoder ABI")
    return()
  endif()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc-api failed at QP ${qp} (${rc}): ${eout}")
  endif()

  # The reference, at the settings the ABI fixes.  Every minor-6 tool named and
  # turned off: naming them is what makes the identity a claim rather than a
  # coincidence, since nxvc_config_default() has four of them on.
  execute_process(COMMAND ${NXVENC} --in ${picked} --w ${pw} --h ${ph}
                          --pix yuv420p --qp ${qp} --frames 3
                          --nsub 3 --matrix 1 --wm 0 --tskip off
                          --chroma-qp-off 0 --ctx v3
                          --eyes 1 --intra-dir off --quiet
                          --no-rdo ${REF_ENTROPY}
                          --split4x4 off --cfl off --xform 8
                          --out ${WORKDIR}/ref.nxv
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-enc failed at QP ${qp} (${rc})")
  endif()

  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/ref.nxv ${WORKDIR}/api.nxv
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
            "QP ${qp}: ${WHAT} is not byte-identical to nxv-enc's stream")
  endif()

  # And it has to decode.  A stream can be byte-identical to a reference stream
  # that the decoder also refuses, so this is not redundant.
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/api.nxv
                          --out ${WORKDIR}/api.yuv --pix yuv420p --quiet
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "QP ${qp}: nxv-dec refused the library's stream (${rc})")
  endif()

  math(EXPR nchecked "${nchecked} + 1")
endforeach()

message(STATUS
        "${WHAT}: byte-identical to nxv-enc at ${nchecked} quantisers "
        "(${pw}x${ph}, 3 frames each)")
