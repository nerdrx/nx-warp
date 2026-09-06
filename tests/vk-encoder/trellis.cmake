# trellis.cmake -- the RD trellis in exact integers.
#
# SPDX-License-Identifier: Apache-2.0
#
# The trellis was ruled uncrossable (vk/encoder/README.md, "Why there is no
# level 2") for two reasons: `table_set_cost` is a sum of `std::log2` terms,
# and the trellis walks a serial chain over the scan.  The first is gone --
# `RateCost` was always Q10 integers and `nxe_neglog2.inc` removes the libm
# that built it -- and the second was never the problem for an encoder that
# runs on a desktop GPU with 64 lanes and one block per lane.
#
# `--int-trellis 1` is the same trellis with the arithmetic replaced: the
# distortion becomes the decoder's own `orig - dequant(m, step)`, the
# accumulator becomes i64 with no division, and the lambda becomes
# `(901 * t * t) >> 12`.  What this pins is that the replacement did not
# change the ANSWER:
#
#   1. the integer trellis produces a stream `nxv-dec` decodes, and
#   2. it stays within a stated distance of the double trellis's rate at every
#      quantiser -- not byte-identical, which it cannot be and is not trying to
#      be, but the same encoder decision to within a fraction of a percent.
#
# (2) is the claim that matters.  An integer trellis that quietly gave up half
# the trellis's gain would still decode, still pass every conformance vector,
# and be worth nothing.
#
# Expects VKENC (for the fixture only), NXVENC, NXVDEC, WORKDIR.

cmake_minimum_required(VERSION 3.22)

if(NOT VKENC OR NOT NXVENC OR NOT NXVDEC OR NOT WORKDIR)
  message(FATAL_ERROR "trellis.cmake: VKENC/NXVENC/NXVDEC/WORKDIR are required")
endif()

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

# The encoder writes its own fixture, as effort.cmake and rate.cmake do, so
# this test carries none and cannot be given different material from them.
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
list(GET fx 4 FRAMES)

set(COMMON --in ${YUV} --w ${W} --h ${H} --frames ${FRAMES} --pix yuv420p
           --nsub 3 --matrix 1 --wm 0 --tskip off --chroma-qp-off 0 --ctx v3
           --eyes 1 --intra-dir off --quiet --split4x4 off --cfl off
           --xform 8 --entropy rans --sign-hide)
function(encode out qp extra)
  execute_process(COMMAND ${NXVENC} ${COMMON} --qp ${qp} ${extra}
                          --out ${WORKDIR}/${out}.nxv
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxv-enc failed for ${out}: ${rc}\n${o}${e}")
  endif()
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/${out}.nxv
                          --out ${WORKDIR}/${out}.yuv --pix yuv420p --quiet
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${out}: the integer trellis produced a stream nxv-dec "
                        "will not decode")
  endif()
endfunction()

# 2 %: the two trellises make the same decision on almost every unit and
# differ only where the double form's `a - m * step / 16` and the integer
# form's `orig - dequant(m, step)` straddle a tie.  Measured over pan8 and
# pan8s at five quantisers the BD-rate difference is under 0.02 %, and the
# per-quantiser byte counts are within a few tenths of a percent; 2 % is that
# with room, so the test fails on a broken trellis and not on a tie.
set(TOL 2)
set(fail 0)
foreach(qp 22 30 40)
  encode(flt${qp} ${qp} "--rdoq-effort;3")
  encode(int${qp} ${qp} "--rdoq-effort;3;--int-trellis;1")
  file(SIZE ${WORKDIR}/flt${qp}.nxv sz_f)
  file(SIZE ${WORKDIR}/int${qp}.nxv sz_i)
  math(EXPR d "(${sz_i} - ${sz_f}) * 100 / ${sz_f}")
  if(d GREATER TOL OR d LESS -${TOL})
    message(SEND_ERROR "QP ${qp}: integer trellis ${sz_i} B against double "
                       "${sz_f} B (${d}%), outside ${TOL}%")
    set(fail 1)
  else()
    message(STATUS "PASS QP ${qp}: int ${sz_i} B vs double ${sz_f} B (${d}%)")
  endif()
endforeach()

# And it has to BEAT the requantiser, or it is not a trellis.
encode(e1 30 "--no-rdo;--int-rdoq;1")
file(SIZE ${WORKDIR}/e1.nxv sz_e1)
file(SIZE ${WORKDIR}/int30.nxv sz_i30)
if(NOT sz_i30 LESS sz_e1)
  message(SEND_ERROR "the integer trellis (${sz_i30} B) does not beat effort 1 "
                     "(${sz_e1} B) at QP 30")
  set(fail 1)
else()
  message(STATUS "PASS int trellis ${sz_i30} B beats effort 1 ${sz_e1} B")
endif()

if(fail)
  message(FATAL_ERROR "trellis.cmake: the integer trellis is not the trellis")
endif()
message(STATUS "trellis.cmake: the integer trellis matches the double one")
