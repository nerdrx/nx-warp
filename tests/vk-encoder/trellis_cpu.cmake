# trellis_cpu.cmake -- effort 2's CPU model against the reference.
#
# SPDX-License-Identifier: Apache-2.0
#
# The reference is the specification of what this encoder produces, so the
# claim is byte-identity and not resemblance: `nxvc-vkenc --cpu --trellis 1`
# must be `nxv-enc --int-trellis 1 --rdoq-effort 3` at the matching flags, on
# both entropy coders and at every quantiser.
#
# It is a sharper test than it looks.  The trellis is only half of it -- the
# other half is the ORDER the two encoders quantise and train in, and three
# separate mismatches there each produced a stream that decoded perfectly and
# was the wrong size:
#
#   * the first pass has to run the TRELLIS, not the dead-zone quantiser, or
#     the table sets are trained on coefficients the reference never saw;
#   * each tile picks its table set from a PLAIN quantisation of itself before
#     the trellis prices anything (ref's inner two-pass), not from the QP seed;
#   * the final per-tile choice is made against the TRAINED sets without
#     restoring the built-in ones first, which is what the training pass does
#     and the emit pass must not;
#   * and restoring the built-in sets means restoring their LOGS too.  The
#     per-tile choice scores through `f.log_freq`, a hoisted `std::log2` that
#     writing `f.tabs` does not rebuild, so the first pass of frame N was
#     scoring frame N-1's trained tables while reading frame N's built-in ones.
#     That one takes SEVEN FRAMES of training to show, which is why this test
#     runs eight.
#
# Runs on the CPU models, on a real device, and on lavapipe: byte-identity is a
# claim about the arithmetic, not about RADV, and the 64-bit accumulator is
# carried as two uints through umulExtended/uaddCarry precisely so that no ICD
# has to offer shaderInt64 for the claim to be checkable on it.
#
# Expects VKENC, NXVENC, WORKDIR, DEVICE.

cmake_minimum_required(VERSION 3.22)

if(NOT VKENC OR NOT NXVENC OR NOT WORKDIR)
  message(FATAL_ERROR "trellis_cpu.cmake: VKENC/NXVENC/WORKDIR are required")
endif()
if(NOT DEVICE)
  set(DEVICE cpu)
endif()
if(DEVICE STREQUAL "cpu")
  set(DEVARGS --cpu)
else()
  set(DEVARGS --device ${DEVICE})
  execute_process(COMMAND ${VKENC} --list RESULT_VARIABLE rc OUTPUT_QUIET
                  ERROR_QUIET)
  if(rc EQUAL 77)
    message(STATUS "SKIP: no Vulkan ICD or no physical device")
    return()
  endif()
endif()

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

execute_process(COMMAND ${VKENC} --dump-inter ${WORKDIR}/
                OUTPUT_VARIABLE fixture RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc --dump-inter failed: ${rc}")
endif()
string(STRIP "${fixture}" fixture)
string(REPLACE " " ";" fx "${fixture}")
list(GET fx 0 YUV)
list(GET fx 2 W)
list(GET fx 3 H)

# Eight frames, deliberately.  Three would pass over a stale-table bug that
# needs seven frames of training to become visible, and did.
set(COMMON --in ${YUV} --w ${W} --h ${H} --frames 8 --pix yuv420p --nsub 3
           --matrix 1 --wm 0 --tskip off --chroma-qp-off 0 --ctx v3 --eyes 1
           --intra-dir off --quiet)
set(MINOR6 --split4x4 off --cfl off --xform 8)
set(RANS_REF --entropy rans --sign-hide --custom-tables --tab v2)
set(RANS_GPU --custom-tables --tab v2)
set(LITE_REF --entropy lite-fixed --no-sign-hide --no-custom-tables --tab v1)
set(LITE_GPU --entropy lite)

set(fail 0)
foreach(qp 22 26 30 34 40)
  foreach(ent rans lite)
    if(ent STREQUAL "rans")
      set(eref ${RANS_REF})
      set(egpu ${RANS_GPU})
    else()
      set(eref ${LITE_REF})
      set(egpu ${LITE_GPU})
    endif()
    execute_process(COMMAND ${NXVENC} ${COMMON} --qp ${qp} ${MINOR6} ${eref}
                            --int-trellis 1 --rdoq-effort 3
                            --out ${WORKDIR}/${ent}${qp}.ref
                    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE e)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "nxv-enc failed (${ent} QP ${qp}): ${rc}\n${e}")
    endif()
    execute_process(COMMAND ${VKENC} ${DEVARGS} ${COMMON} --qp ${qp} ${egpu}
                            --trellis 1 --out ${WORKDIR}/${ent}${qp}.cpu
                    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE e)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "nxvc-vkenc failed (${ent} QP ${qp}): ${rc}\n${e}")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                            ${WORKDIR}/${ent}${qp}.ref
                            ${WORKDIR}/${ent}${qp}.cpu
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      file(SIZE ${WORKDIR}/${ent}${qp}.ref sz_r)
      file(SIZE ${WORKDIR}/${ent}${qp}.cpu sz_c)
      message(SEND_ERROR
        "${ent} QP ${qp} on ${DEVICE}: effort 2 is not byte-identical to "
        "nxv-enc --int-trellis 1 --rdoq-effort 3 (${sz_c} B against ${sz_r} B). "
        "An effort level that is not byte-identical is not an effort level; it "
        "is a different encoder.")
      set(fail 1)
    else()
      message(STATUS "PASS ${ent} QP ${qp}: byte-identical")
    endif()
  endforeach()
endforeach()

if(fail)
  message(FATAL_ERROR "trellis_cpu.cmake: effort 2 is not the reference's trellis")
endif()
# And the stream a device produced has to DECODE, through both decoders, to the
# same pixels: two encoders can agree byte for byte and both be wrong.
if(NXVDEC AND NOT DEVICE STREQUAL "cpu")
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/rans30.cpu
                          --out ${WORKDIR}/rans30.yuv --pix yuv420p --quiet
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-dec will not decode an effort-2 stream")
  endif()
  if(VKDEC)
    execute_process(COMMAND ${VKDEC} --in ${WORKDIR}/rans30.cpu
                            --out ${WORKDIR}/rans30.gpu.yuv --pix yuv420p
                    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    if(rc EQUAL 0)
      execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                              ${WORKDIR}/rans30.yuv ${WORKDIR}/rans30.gpu.yuv
                      RESULT_VARIABLE rc)
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR
          "nxv-dec and nxvc-vkdec disagree about an effort-2 stream")
      endif()
      message(STATUS "PASS both decoders agree on the effort-2 stream")
    endif()
  endif()
endif()
message(STATUS "trellis_cpu.cmake: effort 2 matches nxv-enc byte for byte")
