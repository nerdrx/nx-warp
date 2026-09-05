# acid.cmake -- the encoder's acid test, run by ctest as `cmake -P`.
#
# SPDX-License-Identifier: Apache-2.0
#
# For every configuration `nxvc-vkenc --selftest` covers that the reference
# encoder can also express, on the very same synthesized picture:
#
#   1. the GPU pipeline's stream must be byte-identical to `nxv-enc --no-rdo
#      --intra-dir off --no-custom-tables --split4x4 off --cfl off --tab v1
#      --xform 8 --entropy rans`, with `--ctx` v1, v2 or v3 per configuration
#      -- so every bitstream-minor-6 tool is named, and every one the encoder
#      does not implement is refused rather than merely absent, and
#   2. `nxv-dec` must decode the two to identical pixels.
#
# (1) implies (2), which is the point: checking both says so out loud, and if
# ever they disagree the second line tells you the difference was in the
# bitstream rather than in the decode.
#
# Variables: VKENC, NXVENC, NXVDEC, WORKDIR, DEVICE ("cpu" or an index).

if(NOT VKENC OR NOT NXVENC OR NOT NXVDEC OR NOT WORKDIR)
  message(FATAL_ERROR "acid.cmake: VKENC/NXVENC/NXVDEC/WORKDIR are required")
endif()
if(NOT DEFINED DEVICE)
  set(DEVICE 0)
endif()

file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

# The tool writes one YUV per configuration and prints a line of parameters for
# each, so the two encoders are driven from one description and cannot drift.
execute_process(COMMAND ${VKENC} --dump-selftest-yuv ${WORKDIR}/
                OUTPUT_VARIABLE cases RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc --dump-selftest-yuv failed: ${rc}")
endif()

if(DEVICE STREQUAL "cpu")
  set(DEVARGS --cpu)
else()
  set(DEVARGS --device ${DEVICE})
  # `cmake -P` cannot choose its own exit code, so the skip is signalled in the
  # output and the test carries SKIP_REGULAR_EXPRESSION for it.
  execute_process(COMMAND ${VKENC} --list RESULT_VARIABLE rc OUTPUT_QUIET
                  ERROR_QUIET)
  if(rc EQUAL 77)
    message(STATUS "SKIP: no Vulkan ICD or no physical device")
    return()
  endif()
endif()

string(REPLACE "\n" ";" lines "${cases}")
set(npass 0)
set(nskip 0)
foreach(line ${lines})
  if(line STREQUAL "")
    continue()
  endif()
  string(REPLACE " " ";" f "${line}")
  list(GET f 0 path)
  list(GET f 1 w)
  list(GET f 2 h)
  list(GET f 3 eyes)
  list(GET f 4 pix)
  list(GET f 5 qp)
  list(GET f 6 mx)
  list(GET f 7 wm)
  list(GET f 8 ns)
  list(GET f 9 ts)
  list(GET f 10 cq)
  list(GET f 11 ctxlevel)   # the entropy context model, 1/2/3
  list(GET f 12 sdh)
  list(GET f 13 dir)
  list(GET f 15 seed)
  list(GET f 16 frames)
  list(GET f 17 ct)         # tool bit 6, CUSTOM_TABLES
  list(GET f 18 tab)        # tool bit 26, TAB_V2 (needs bit 6)

  # The reference encoder searches its own per-block intra modes; this pipeline
  # takes them as an input.  A directional configuration therefore has no
  # reference stream to be identical to -- `--selftest` covers it against the
  # CPU models and against its pinned digest instead.
  if(dir STREQUAL "1")
    math(EXPR nskip "${nskip} + 1")
    continue()
  endif()

  # Tools 21 (CTX_V2) and 25 (CTX_V3), as one level rather than two flags.
  set(ctx v${ctxlevel})
  set(sh --sign-hide)
  if(sdh STREQUAL "0")
    set(sh --no-sign-hide)
  endif()
  set(tsk off)
  if(ts STREQUAL "1")
    set(tsk on)
  endif()
  # Custom tables and the compact table set.  Both encoders take the same two
  # flags and the same default table_iters, so a configuration that trains
  # tables is compared byte for byte like any other -- the training is host
  # work in both, over the same histogram, in the same double precision.
  set(cts --no-custom-tables)
  if(ct STREQUAL "1")
    set(cts --custom-tables)
  endif()
  set(tabf --tab v1)
  if(tab STREQUAL "2")
    set(tabf --tab v2)
  endif()
  set(common --in ${path} --w ${w} --h ${h} --pix ${pix} --qp ${qp}
             --frames ${frames} --nsub ${ns} --matrix ${mx} --wm ${wm}
             --tskip ${tsk} --chroma-qp-off ${cq} --ctx ${ctx} ${sh}
             ${cts} ${tabf}
             --eyes ${eyes} --intra-dir off --quiet)

  # Every bitstream-minor-6 tool, named and turned off explicitly.
  #
  # Naming them matters more than it looks.  Four of the six are ON in
  # `nxvc_config_default()` or would be reached by a default the reference
  # encoder is free to change, and three of them are only off here as a *side
  # effect* of `--intra-dir off`: `ref/src/codec_impl.inc` gates `split4` on
  # `intra_dir` and `cfl` on `intra_dir && ctx_v2 && !dir_layer`, so a
  # directional configuration is the only one where either can appear.  This
  # test would therefore have kept passing while silently covering a smaller
  # stream than it claims to.  `TAB_V2` is likewise gated on
  # `custom_tables`, which is already off, and `ENTROPY_LITE` and
  # `XFORM_LARGE` are off by default today and are follow-ups on the encoder
  # side (see vk/encoder/README.md).
  #
  # The GPU pipeline implements none of the six, so this is the flag set that
  # makes byte-identity a meaningful claim rather than a coincidence.
  set(minor6_off --split4x4 off --cfl off --xform 8 --entropy rans)

  execute_process(COMMAND ${NXVENC} ${common} --out ${WORKDIR}/ref.nxv
                          --no-rdo ${minor6_off}
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${path}: nxv-enc failed (${rc})")
  endif()
  execute_process(COMMAND ${VKENC} ${common} --out ${WORKDIR}/gpu.nxv ${DEVARGS}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE eout)
  if(rc EQUAL 77)
    file(REMOVE_RECURSE ${WORKDIR})
    message(STATUS "SKIP: no usable Vulkan device for this configuration")
    return()
  endif()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${path}: nxvc-vkenc failed (${rc}): ${eout}")
  endif()

  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/ref.nxv ${WORKDIR}/gpu.nxv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${path}: the stream is not byte-identical to nxv-enc")
  endif()

  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/ref.nxv
                          --out ${WORKDIR}/ref.yuv --quiet
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${path}: nxv-dec refused the reference stream")
  endif()
  execute_process(COMMAND ${NXVDEC} --in ${WORKDIR}/gpu.nxv
                          --out ${WORKDIR}/gpu.yuv --quiet
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${path}: nxv-dec refused the GPU stream")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                          ${WORKDIR}/ref.yuv ${WORKDIR}/gpu.yuv
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${path}: the decoded pixels differ")
  endif()

  message(STATUS "ok  ${path}")
  math(EXPR npass "${npass} + 1")
endforeach()

file(REMOVE_RECURSE ${WORKDIR})
if(npass EQUAL 0)
  message(FATAL_ERROR "acid.cmake: no configuration ran")
endif()
message(STATUS "${npass} configurations byte-identical and pixel-identical, "
               "${nskip} directional configurations covered by --selftest")
