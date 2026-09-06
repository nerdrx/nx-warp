# planar_acid.cmake -- the planar tile mode, byte-identical to the reference.
#
# [SYN] 13.13 on the GPU encoder: the fit (nxe_planar_host.h, which ref/
# includes too) and the emission (E4's header, E5's raw body out of binding 9).
#
# WHAT THIS PINS, and what it does not.  The configuration SEARCH and the
# rate-distortion gate are not wired yet -- they need the INTRA cost, which on
# this pipeline is not known until E3 and E4 have run -- so both encoders are
# driven with the two development hooks ref/ already has:
# NXVC_PLANAR_CONFIG pins one of the six configurations and NXVC_PLANAR_FORCE
# takes the mode on every eligible tile.  With the decision removed from both
# sides what remains is the fit and the bytes, and that is what is compared.
#
# All six configurations are swept -- two granularities by three region counts
# -- because the map size, the label packing and the coefficient count all
# change with them, and a single configuration would pin one shape of body.
cmake_minimum_required(VERSION 3.20)

set(WORKDIR "${CMAKE_CURRENT_BINARY_DIR}/planar_acid")
file(MAKE_DIRECTORY "${WORKDIR}")

execute_process(COMMAND ${VKENC} --dump-selftest-yuv ${WORKDIR}/
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxvc-vkenc --dump-selftest-yuv failed: ${rc}")
endif()

if(DEVICE STREQUAL "cpu")
  set(DEVARGS --cpu)
else()
  set(DEVARGS --device ${DEVICE})
endif()

set(npass 0)
foreach(fx "420-qp24;256;192;yuv420p;24" "444-qp20;256;192;yuv444p;20")
  list(GET fx 0 nm)
  list(GET fx 1 w)
  list(GET fx 2 h)
  list(GET fx 3 pix)
  list(GET fx 4 qp)
  if(NOT EXISTS "${WORKDIR}/${nm}.yuv")
    continue()
  endif()
  foreach(cfg "2,0" "3,0" "4,0" "2,1" "3,1" "4,1")
    set(ENV{NXVC_PLANAR_FORCE} 1)
    set(ENV{NXVC_PLANAR_CONFIG} ${cfg})
    execute_process(
      COMMAND ${REFENC} --in ${WORKDIR}/${nm}.yuv --w ${w} --h ${h} --pix ${pix}
              --qp ${qp} --frames 1 --nsub 3 --no-rdo --no-custom-tables
              --intra-dir off --split4x4 off --cfl off --tab v1 --xform 8
              --entropy rans --planar --out ${WORKDIR}/ref.nxv
      RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "${nm} ${cfg}: nxv-enc failed (${rc})")
    endif()
    execute_process(
      COMMAND ${VKENC} --in ${WORKDIR}/${nm}.yuv --w ${w} --h ${h} --pix ${pix}
              --qp ${qp} --frames 1 ${DEVARGS} --planar
              --out ${WORKDIR}/gpu.nxv
      RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "${nm} ${cfg}: nxvc-vkenc failed (${rc})")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
                    ${WORKDIR}/ref.nxv ${WORKDIR}/gpu.nxv
                    RESULT_VARIABLE same)
    if(NOT same EQUAL 0)
      message(FATAL_ERROR
              "${nm} regions/fine ${cfg}: the planar stream is not "
              "byte-identical to nxv-enc")
    endif()
    math(EXPR npass "${npass} + 1")
  endforeach()
endforeach()
message(STATUS "planar: ${npass} stream(s) byte-identical to the reference")
