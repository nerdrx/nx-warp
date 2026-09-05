# qp_switch.cmake -- the acid test for a quantiser that MOVES.
#
# SPDX-License-Identifier: Apache-2.0
#
# api_acid.cmake pins byte identity with `nxv-enc` at a fixed quantiser: one
# encoder, created at QP q, coding a whole sequence at q.  That says nothing
# about the entry point a rate controller uses, which is
# nxvc_vk_encoder_set_qp() between frames -- and "between frames" is exactly
# where a stale cache would hide.  An encoder that carried anything of the
# previous quantiser (a frame-parameter buffer uploaded once, a table-set seed
# left where setup() put it, a descriptor bound at create) would still produce
# a plausible stream, still decode, and be wrong only by a few bytes a frame.
#
# So this checks the strongest thing that can be checked: after set_qp(q),
# every frame is byte for byte the frame `nxv-enc --qp q` codes AT THAT FRAME
# NUMBER.  Nothing of the previous quantiser survives the call.
#
# It is a frame-by-frame comparison, so it needs the frame boundaries of both
# files.  Those come from `nxvc-vkenc-api --lengths`, and using the encoder for
# the LAYOUT is not circular: every reference file's layout is validated first
# by comparing the whole file against nxv-enc's at that quantiser, which is
# api_acid's claim, re-run here because these particular files need it.
#
# Required: APIENC, NXVENC, NXVDEC, VKENC, WORKDIR.  Skips (via "SKIP:" in the
# output, which the ctest carries a SKIP_REGULAR_EXPRESSION for) when there is
# no usable Vulkan device.

cmake_minimum_required(VERSION 3.22)

if(NOT APIENC OR NOT NXVENC OR NOT NXVDEC OR NOT VKENC OR NOT WORKDIR)
  message(FATAL_ERROR
          "qp_switch.cmake: APIENC/NXVENC/NXVDEC/VKENC/WORKDIR are required")
endif()

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

# ---------------------------------------------------------------- the source
# The same picture api_acid.cmake uses, for the same reason: `nxv-enc --gen` is
# not a thing and CMake cannot synthesize a frame at any useful speed.
execute_process(COMMAND ${VKENC} --dump-selftest-yuv ${WORKDIR}/
                OUTPUT_VARIABLE cases RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc --dump-selftest-yuv failed: ${rc}")
endif()

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
  list(GET f 16 nfr)
  if(eyes STREQUAL "1" AND pix STREQUAL "yuv420p" AND nfr GREATER 1)
    list(GET f 0 picked)
    set(pw ${w})
    set(ph ${h})
    break()
  endif()
endforeach()
if(picked STREQUAL "")
  message(FATAL_ERROR "qp_switch.cmake: no multi-frame single-eye 4:2:0 case")
endif()

# The dumped cases are one or two frames long, which is not enough sequence for
# a quantiser to move over.  Repeat the file: the frames are intra and carry
# their own frame number, so a repeated picture is still a distinct frame and
# still has to match the reference at its own index.
set(SRC ${WORKDIR}/src.yuv)
execute_process(COMMAND ${CMAKE_COMMAND} -E cat
                        ${picked} ${picked} ${picked} ${picked}
                OUTPUT_FILE ${SRC} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "could not build the repeated source (${rc})")
endif()

# The quantiser per frame.  Deliberately not monotonic and deliberately
# repeating: a controller ramps both ways and revisits values, and a jump of 24
# QP (20 <-> 44) is the widest a clamp in the caller would ever allow.
set(CYCLE 20 44 26 30 44 20 26 44)
list(LENGTH CYCLE NWANT)
set(CYCLE_ARG "")
foreach(q ${CYCLE})
  if(CYCLE_ARG STREQUAL "")
    set(CYCLE_ARG "${q}")
  else()
    set(CYCLE_ARG "${CYCLE_ARG},${q}")
  endif()
endforeach()

# The quantiser the encoders are CREATED at.  In every run below it is a value
# that appears nowhere in the cycle, so a frame that matched only because the
# create-time QP leaked through would not match anything.
set(CREATE_QP 33)

# ------------------------------------------------------------------- helpers
# The lengths file is the header length on the first line and one frame length
# per line after it.
function(read_layout path hdr_out lens_out)
  file(STRINGS ${path} L)
  list(GET L 0 h)
  list(REMOVE_AT L 0)
  set(${hdr_out} ${h} PARENT_SCOPE)
  set(${lens_out} ${L} PARENT_SCOPE)
endfunction()

# Byte offset of frame `idx` in a file with this layout.
function(frame_offset hdr lens idx out)
  set(off ${hdr})
  set(i 0)
  foreach(l ${lens})
    if(i GREATER_EQUAL ${idx})
      break()
    endif()
    math(EXPR off "${off} + ${l}")
    math(EXPR i "${i} + 1")
  endforeach()
  set(${out} ${off} PARENT_SCOPE)
endfunction()

# ----------------------------------------------- the per-quantiser references
# One reference stream per distinct quantiser in the cycle, each the whole
# sequence at that quantiser, each proved against nxv-enc as a whole file
# before any frame of it is used as a reference for anything.
set(DISTINCT ${CYCLE})
list(REMOVE_DUPLICATES DISTINCT)
list(SORT DISTINCT)

foreach(q ${DISTINCT})
  # The library at a constant q, which is also where this file's layout comes
  # from.  Created at CREATE_QP and moved to q by set_qp before every frame:
  # setting the quantiser the encoder already has must be a no-op, so a
  # constant "cycle" has to reproduce the constant-QP stream exactly.
  execute_process(COMMAND ${APIENC} --in ${SRC} --w ${pw} --h ${ph}
                          --qp ${CREATE_QP} --qp-cycle ${q}
                          --frames ${NWANT} --matrix 1
                          --lengths ${WORKDIR}/len-${q}.txt
                          --out ${WORKDIR}/api-${q}.nxv
                  RESULT_VARIABLE rc ERROR_VARIABLE eout)
  if(rc EQUAL 77)
    message(STATUS "SKIP: no usable Vulkan device for the encoder ABI")
    return()
  endif()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc-api failed at QP ${q} (${rc}): ${eout}")
  endif()

  # The reference, at the settings the ABI fixes -- the same command line
  # api_acid.cmake uses, every bitstream minor-6 tool named and turned off.
  execute_process(COMMAND ${NXVENC} --in ${SRC} --w ${pw} --h ${ph}
                          --pix yuv420p --qp ${q} --frames ${NWANT}
                          --nsub 3 --matrix 1 --wm 0 --tskip off
                          --chroma-qp-off 0 --ctx v2 --sign-hide
                          --eyes 1 --intra-dir off --quiet
                          --no-rdo --no-custom-tables
                          --split4x4 off --cfl off --tab v1 --xform 8
                          --entropy rans
                          --out ${WORKDIR}/ref-${q}.nxv
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-enc failed at QP ${q} (${rc})")
  endif()

  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/ref-${q}.nxv ${WORKDIR}/api-${q}.nxv
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
            "set_qp(${q}) from a create-time QP of ${CREATE_QP} does not "
            "reproduce nxv-enc's stream at QP ${q}")
  endif()
endforeach()

# ------------------------------------------------------------ the mixed stream
execute_process(COMMAND ${APIENC} --in ${SRC} --w ${pw} --h ${ph}
                        --qp ${CREATE_QP} --qp-cycle ${CYCLE_ARG}
                        --frames ${NWANT} --matrix 1
                        --lengths ${WORKDIR}/len-mix.txt
                        --out ${WORKDIR}/mix.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE eout)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc-api failed on the mixed cycle (${rc}): ${eout}")
endif()

read_layout(${WORKDIR}/len-mix.txt MIXHDR MIXLENS)
list(LENGTH MIXLENS NGOT)
if(NOT NGOT EQUAL NWANT)
  message(FATAL_ERROR "the mixed run coded ${NGOT} frames, wanted ${NWANT}")
endif()

# --------------------------------------------------------- frame by frame
set(nchecked 0)
set(idx 0)
foreach(q ${CYCLE})
  read_layout(${WORKDIR}/len-${q}.txt RHDR RLENS)
  if(NOT RHDR EQUAL MIXHDR)
    message(FATAL_ERROR
            "the stream header changed with the quantiser (${RHDR} vs ${MIXHDR} "
            "bytes); a decoder parses it once, before any frame")
  endif()

  list(GET MIXLENS ${idx} mlen)
  list(GET RLENS ${idx} rlen)
  if(NOT mlen EQUAL rlen)
    message(FATAL_ERROR
            "frame ${idx} at QP ${q}: the mixed stream spent ${mlen} bytes, "
            "the constant-QP stream ${rlen}")
  endif()

  frame_offset(${MIXHDR} "${MIXLENS}" ${idx} moff)
  frame_offset(${RHDR} "${RLENS}" ${idx} roff)
  file(READ ${WORKDIR}/mix.nxv     mbytes OFFSET ${moff} LIMIT ${mlen} HEX)
  file(READ ${WORKDIR}/ref-${q}.nxv rbytes OFFSET ${roff} LIMIT ${rlen} HEX)
  if(NOT mbytes STREQUAL rbytes)
    message(FATAL_ERROR
            "frame ${idx} of the mixed stream is not the frame nxv-enc codes "
            "at QP ${q} and frame number ${idx}")
  endif()

  math(EXPR nchecked "${nchecked} + 1")
  math(EXPR idx "${idx} + 1")
endforeach()

# And the mixed stream has to decode.  A stream can be byte-identical to a set
# of reference frames and still be refused as a sequence -- a decoder carries
# state across frames that none of the per-frame comparisons above can see.
execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/mix.nxv
                        --out ${WORKDIR}/mix.yuv --pix yuv420p --quiet
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-dec refused the mixed-quantiser stream (${rc})")
endif()

list(LENGTH DISTINCT ndist)
message(STATUS
        "nxvc_vk_encoder_set_qp: ${nchecked} frames over ${ndist} quantisers "
        "(${CYCLE}) each byte-identical to nxv-enc at that frame's QP "
        "(${pw}x${ph}, created at QP ${CREATE_QP})")
