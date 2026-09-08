# PLANAR with INTER, for both entropy payload paths.  The decoder walk is
# intentional: a successful encoder exit alone does not prove mode 5 syntax.
cmake_minimum_required(VERSION 3.20)
set(WORKDIR "${CMAKE_CURRENT_BINARY_DIR}/planar-inter-lite")
file(MAKE_DIRECTORY "${WORKDIR}")
execute_process(COMMAND ${VKENC} --dump-selftest-yuv ${WORKDIR}/
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "selftest fixture failed: ${rc}")
endif()
if(DEFINED DEVICE AND DEVICE STREQUAL "cpu")
  set(DEVARGS --cpu)
else()
  set(DEVARGS --device 0)
endif()
foreach(ent rans lite)
  foreach(cfg "2,0" "3,0" "4,0" "2,1" "3,1" "4,1")
    set(ENV{NXVC_PLANAR_FORCE} 1)
    set(ENV{NXVC_PLANAR_CONFIG} ${cfg})
    set(out "${WORKDIR}/${ent}-${cfg}.nxv")
    execute_process(COMMAND ${VKENC} --in ${WORKDIR}/420-qp24.yuv
      --w 256 --h 192 --pix yuv420p --qp 24 --frames 2 --inter --atlas
      --entropy ${ent} --planar --out ${out} ${DEVARGS}
      RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "${ent} ${cfg}: encoder failed: ${err}")
    endif()
    execute_process(COMMAND ${NXVDEC} --in ${out} --out ${WORKDIR}/decoded.yuv
                    RESULT_VARIABLE rc OUTPUT_VARIABLE decout ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "${ent} ${cfg}: ref decoder rejected stream: ${err}")
    endif()
    if(NOT decout MATCHES "2 frame\\(s\\)")
      message(FATAL_ERROR "${ent} ${cfg}: decoder did not report 2 frames")
    endif()
    execute_process(COMMAND ${NXVINFO} --in ${out} --modes
                    OUTPUT_VARIABLE info RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0 OR NOT info MATCHES "PLANAR[ ]+24([ ]|$)")
      message(FATAL_ERROR "${ent} ${cfg}: expected 24 PLANAR tiles in nxv-info")
    endif()
  endforeach()
endforeach()
message(STATUS "PLANAR inter/rANS+Lite syntax tests passed")
