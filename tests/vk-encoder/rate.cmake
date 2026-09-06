# rate.cmake -- the integer rate model against the coder that has to agree.
#
# SPDX-License-Identifier: Apache-2.0
#
# `nxe_rate.h` prices a tile from the operation list E4 would encode, in
# integers, with a table lookup where the reference uses `std::log2`.  Two
# claims are made for it and both rot silently if nothing checks them:
#
#   1. under ENTROPY_LITE it is EXACT.  Lite has no arithmetic coder, so a
#      tile's payload is a sum of fixed field widths and five align-to-byte
#      roundings; the model computes that sum and must equal the coder's own
#      byte count to the bit, on every tile.  Not "within a tolerance" -- 0.
#
#   2. under rANS it is within a stated tolerance.  The symbol part is exact
#      (it is the coder's own operation list under the coder's own table);
#      what is not exact is rANS's sub-bit rounding and the 32-bit state flush
#      per lane, which nxe_rate.h models at 24 bits.  The bound here is the
#      measured spread with headroom, not a fitted number -- see
#      vk/encoder/README.md "The integer rate model, measured".
#
# The CPU models are the specification (vk/encoder/README.md "Bit-exactness"),
# so this runs `--cpu` and needs no Vulkan at all.
#
# Expects VKENC and WORKDIR.

cmake_minimum_required(VERSION 3.22)

if(NOT VKENC OR NOT WORKDIR)
  message(FATAL_ERROR "rate.cmake: VKENC/WORKDIR are required")
endif()

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

# The encoder writes its own fixture, exactly as effort.cmake does, so the two
# tests cannot be given different material and neither needs one checked in.
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
           --eyes 1 --matrix 1 --wm 0 --tskip off --intra-dir off --rate-check)

# Pull "mean |err| X%" and "total +Y%" out of a run.
function(run_rate tag)
  execute_process(COMMAND ${VKENC} --cpu ${COMMON} ${ARGN}
                          --out ${WORKDIR}/${tag}.nxv
                  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nxvc-vkenc failed for ${tag}: ${rc}\n${out}${err}")
  endif()
  set(RATE_OUT "${out}" PARENT_SCOPE)
endfunction()

set(fail 0)

# ---- 1. Lite is exact, at every quantiser.
foreach(qp 22 30 40)
  run_rate(lite${qp} --qp ${qp} --entropy lite-fixed --no-sign-hide
                     --no-custom-tables)
  if(NOT RATE_OUT MATCHES "mean \\|err\\| 0\\.000%")
    message(SEND_ERROR "Lite rate model is not exact at QP ${qp}:\n${RATE_OUT}")
    set(fail 1)
  else()
    message(STATUS "PASS lite QP ${qp}: exact")
  endif()
endforeach()

# ---- 2. rANS is within tolerance, at every quantiser.
#
# 3.0 % mean absolute error per tile.  Measured worst over pan8/pan8s at
# QP 22..40 is 1.68 %; the bound is set with headroom because the material
# here is the generator's rather than those clips', and a bound that tracked
# one measurement to the decimal would be a test of the clip.
set(TOL 3.0)
foreach(qp 22 30 40)
  run_rate(rans${qp} --qp ${qp} --ctx v3 --custom-tables --tab v2 --nsub 3)
  if(NOT RATE_OUT MATCHES "mean \\|err\\| ([0-9.]+)%")
    message(SEND_ERROR "no rate report for rANS at QP ${qp}:\n${RATE_OUT}")
    set(fail 1)
  else()
    set(e ${CMAKE_MATCH_1})
    if(e GREATER TOL)
      message(SEND_ERROR "rANS rate model at QP ${qp}: mean |err| ${e}% > ${TOL}%")
      set(fail 1)
    else()
      message(STATUS "PASS rans QP ${qp}: mean |err| ${e}% (bound ${TOL}%)")
    endif()
  endif()
endforeach()

# ---- 3. The rate model's consumer: a forced ladder is the QP it names.
#
# `--qp-ladder -4 --qp-lambda 1` makes the decision pick -4 for every tile of
# every frame, which is "encode at base_qp - 4" by another route.  So it has to
# come out the size of a uniform QP-4 run, and it did not: `Frame::jobs` is
# allocated once and reused for every frame, so a search that read its own
# previous answer back as the base compounded it -- -4 became -8 on frame 2 and
# -16 by frame 4, a 44 % rate rise and 4 dB of PSNR nobody asked for.  A
# single-frame test cannot see that, so this one runs several.
#
# 2 % rather than 0 %: the table-set seed is derived from the frame's base QP,
# so the two runs are not required to be byte-identical, only to be the same
# encode.
run_rate(uniform26 --qp 26 --ctx v3 --custom-tables --tab v2 --nsub 3)
file(SIZE ${WORKDIR}/uniform26.nxv sz_uniform)
run_rate(forced26 --qp 30 --qp-ladder -4 --qp-lambda 1
                  --ctx v3 --custom-tables --tab v2 --nsub 3)
file(SIZE ${WORKDIR}/forced26.nxv sz_forced)
math(EXPR sz_diff "(${sz_forced} - ${sz_uniform}) * 100 / ${sz_uniform}")
if(sz_diff GREATER 2 OR sz_diff LESS -2)
  message(SEND_ERROR
    "a ladder forced to -4 is not the same encode as QP-4: "
    "${sz_forced} against ${sz_uniform} bytes (${sz_diff}%).  The likely cause "
    "is the per-tile decision reading back its own previous frame's answer.")
  set(fail 1)
else()
  message(STATUS "PASS forced ladder == uniform QP-4 (${sz_diff}%)")
endif()

if(fail)
  message(FATAL_ERROR "rate.cmake: the rate model missed its bound")
endif()
message(STATUS "rate.cmake: the rate model agrees with both coders")
