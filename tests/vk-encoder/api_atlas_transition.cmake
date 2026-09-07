cmake_minimum_required(VERSION 3.22)
if(NOT APIENC OR NOT VKENC OR NOT NXVINFO OR NOT WORKDIR)
  message(FATAL_ERROR "api_atlas_transition.cmake: APIENC/VKENC/NXVINFO/WORKDIR required")
endif()
file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})
execute_process(COMMAND ${VKENC} --dump-selftest-yuv ${WORKDIR}
                OUTPUT_VARIABLE dump RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cannot obtain deterministic YUV fixture: ${rc}")
endif()
string(REPLACE "\n" ";" lines "${dump}")
list(GET lines 0 line)
string(REPLACE " " ";" fields "${line}")
list(GET fields 0 one)
list(GET fields 1 width)
list(GET fields 2 height)
execute_process(COMMAND ${CMAKE_COMMAND} -E cat ${one} ${one} ${one}
                OUTPUT_FILE ${WORKDIR}/three.yuv RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cannot concatenate fixture: ${rc}")
endif()
file(WRITE ${WORKDIR}/poses.json
  "{\"orientation_xyzw\":[0,0,0,1]}\n"
  "{\"orientation_xyzw\":[0,0,0.0998334,0.9950042]}\n"
  "{\"orientation_xyzw\":[0,0,0.0998334,0.9950042]}\n")
execute_process(COMMAND ${APIENC} --in ${WORKDIR}/three.yuv
                --w ${width} --h ${height} --qp 24 --frames 3 --inter
                --atlas --atlas-mode --atlas-picture-d 0
                --poses ${WORKDIR}/poses.json --out ${WORKDIR}/out.nxv
                RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  if(rc EQUAL 77)
    message(STATUS "SKIP: no usable Vulkan device")
    return()
  endif()
  message(FATAL_ERROR "API transition encode failed: ${err}")
endif()
execute_process(COMMAND ${NXVINFO} --in ${WORKDIR}/out.nxv
                OUTPUT_VARIABLE info RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nxv-info failed: ${rc}")
endif()
foreach(expected
  "frame 0 @.*flags 0x01"
  "frame 1 @.*flags 0x28"
  "frame 2 @.*flags 0x08")
  if(NOT info MATCHES "${expected}")
    message(FATAL_ERROR "missing expected emitted header '${expected}'\n${info}")
  endif()
endforeach()
message(STATUS "API PICTURE-to-ATLAS transition emitted flags 0x01, 0x28, 0x08")
