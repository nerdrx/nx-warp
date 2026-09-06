# [SYN] 13.12.11: the atlas as a per-frame MODE, and the property that says
# the PICTURE path is really the ordinary path.
#
# A PICTURE frame is decoded "by the ORDINARY non-ATLAS process, in full".  So
# a stream in which EVERY frame is a PICTURE frame must be the stream the
# encoder produces with no atlas at all -- same modes, same vectors, same
# residuals, same bytes.  That is a strong end-to-end check and it is cheap:
# force the trigger with `--atlas-picture-d 0`, which fires on any displacement
# at all, and compare against `--inter` alone.
#
# WHAT MAY LEGALLY DIFFER, and nothing else:
#
#   * the stream header's tools word, which advertises ATLAS (bit 31) and
#     ATLAS_REBASE (bit 34).  Two bytes.
#   * each PICTURE frame's own flags byte, bit 5, which is the frame SAYING
#     which mode it is in.  One byte per PICTURE frame.
#
# The coded payload -- every row header, every tile, every residual byte -- is
# identical, which is what "the same codec at two operating points" means.  The
# check is written as an exact count of differing bytes rather than a size
# comparison, because two streams of the same length can differ everywhere.
#
# This is also the regression test for the ring: the assembled picture needs a
# THIRD ring slot, and an ATLAS encoder allocates two.  Writing the assembly
# past the end of the buffer produced no error anywhere -- it read back as an
# unusable reference, so every tile was coded INTRA and the stream was 43 %
# larger while remaining perfectly valid.  A size comparison alone would have
# caught that one; a byte count catches it and everything subtler.
#
# Expects VKENC, NXVDEC, WORKDIR, DEVICE.

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

if(DEVICE STREQUAL "cpu")
  message(STATUS "SKIP: the atlas has no CPU model yet")
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

set(common --in ${YUV} --w ${W} --h ${H} --pix yuv420p --qp 26
           --frames ${FRAMES} --nsub 3 --matrix 1 --wm 0 --tskip off
           --chroma-qp-off 0 --ctx v3 --eyes 1 --intra-dir off --quiet
           --poses ${POSES} --intra-period 180 --inter --custom-tables
           --tab v2 --device ${DEVICE})

execute_process(COMMAND ${VKENC} ${common} --atlas --atlas-mode
                        --atlas-picture-d 0 --out ${WORKDIR}/all.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE eout)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "all-PICTURE encode failed (${rc}): ${eout}")
endif()
execute_process(COMMAND ${VKENC} ${common} --out ${WORKDIR}/off.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE eout)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "no-atlas encode failed (${rc}): ${eout}")
endif()

file(SIZE ${WORKDIR}/all.nxv sa)
file(SIZE ${WORKDIR}/off.nxv sb)
if(NOT sa EQUAL sb)
  message(FATAL_ERROR
    "an all-PICTURE stream is ${sa} bytes against the no-atlas stream's ${sb}. "
    "13.12.11 makes a PICTURE frame the ordinary process, so the two must be "
    "the same bytes; a larger one means the assembled reference was not "
    "usable and every tile was coded INTRA.")
endif()

# Count the differing bytes, and check each is one of the two kinds that may
# differ.  CMake cannot diff bytes, so the files go through their hex form.
file(READ ${WORKDIR}/all.nxv ha HEX)
file(READ ${WORKDIR}/off.nxv hb HEX)
string(LENGTH "${ha}" n)
set(ndiff 0)
set(nflag 0)
set(ntool 0)
math(EXPR last "${n} / 2 - 1")
foreach(i RANGE ${last})
  math(EXPR o "${i} * 2")
  string(SUBSTRING "${ha}" ${o} 2 ba)
  string(SUBSTRING "${hb}" ${o} 2 bb)
  if(NOT ba STREQUAL bb)
    math(EXPR ndiff "${ndiff} + 1")
    # Bytes 35 and 36 are the stream header's tools word, bits 31 and 34.
    if(i EQUAL 35 OR i EQUAL 36)
      math(EXPR ntool "${ntool} + 1")
    else()
      # Everything else must be a frame's flags byte differing by exactly
      # bit 5, which is the frame declaring itself a PICTURE frame.
      math(EXPR xa "0x${ba}")
      math(EXPR xb "0x${bb}")
      math(EXPR x "${xa} ^ ${xb}")
      if(NOT x EQUAL 32)
        message(FATAL_ERROR
          "byte ${i} differs by 0x${x}, which is not frame flags bit 5. "
          "An all-PICTURE stream may differ from the no-atlas stream only in "
          "the tools word and in each frame's mode bit; a difference in the "
          "coded payload means the PICTURE path is not the ordinary path.")
      endif()
      math(EXPR nflag "${nflag} + 1")
    endif()
  endif()
endforeach()

if(NOT ntool EQUAL 2)
  message(FATAL_ERROR "expected both tools bytes to differ, got ${ntool}")
endif()
if(nflag EQUAL 0)
  message(FATAL_ERROR
    "not one frame declared itself a PICTURE frame, so the trigger never "
    "fired and this test compared two identical configurations.")
endif()

# And the two must decode to the same pixels through the REFERENCE decoder,
# which is what says the mode bit is understood rather than merely tolerated.
execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/all.nxv
                        --out ${WORKDIR}/all.yuv --pix yuv420p --quiet
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-dec refused the all-PICTURE stream (${rc})")
endif()
execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/off.nxv
                        --out ${WORKDIR}/off.yuv --pix yuv420p --quiet
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-dec refused the no-atlas stream (${rc})")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                        ${WORKDIR}/all.yuv ${WORKDIR}/off.yuv
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the two streams decode to different pixels")
endif()

message(STATUS "vk.encoder.atlas.mode: ${sa} bytes both ways, ${ndiff} bytes "
               "differ (${ntool} tools, ${nflag} PICTURE mode bits), decoded "
               "identical")
