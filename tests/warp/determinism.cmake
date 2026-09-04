# NX Warp -- determinism test driver.
#
# Compiles warp_hash.cpp against the reference sources under several
# compilers and optimisation levels, runs each, and requires one identical
# hash from all of them.
#
# The variants deliberately include -ffast-math and -march=native. Neither may
# change anything: there is no floating point on the normative path, and if
# one of them ever does move a bit, that is the bug this test exists to find.
#
# Required: SRC (tests/warp dir), WARP (warp dir), OUT (scratch dir),
#           COMPILERS (";"-separated list of C++ compilers)

set(_srcs
    ${WARP}/ref/warp_ref.cpp
    ${WARP}/ref/homography.cpp
    ${WARP}/ref/warp_oracle.cpp
    ${SRC}/warp_hash.cpp)

set(_flagsets "-O0" "-O1" "-O2" "-O3" "-Os" "-O2;-ffast-math" "-O3;-march=native")

file(MAKE_DIRECTORY ${OUT})
set(_hashes "")
set(_labels "")

foreach(cxx IN LISTS COMPILERS)
    if(NOT cxx)
        continue()
    endif()
    foreach(flags IN LISTS _flagsets)
        string(REPLACE ";" "_" _tag "${flags}")
        string(REPLACE "-" "" _tag "${_tag}")
        get_filename_component(_cxxname ${cxx} NAME)
        set(_exe ${OUT}/hash_${_cxxname}_${_tag})
        string(REPLACE ";" " " _flagstr "${flags}")
        execute_process(
            COMMAND ${cxx} -std=c++20 ${flags} -I${WARP}/include -I${SRC} ${_srcs} -o ${_exe}
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            # -march=native is not universally available; skip rather than fail.
            if(_flagstr MATCHES "march=native")
                message(STATUS "skip ${_cxxname} ${_flagstr} (not supported here)")
                continue()
            endif()
            message(FATAL_ERROR "compile failed: ${_cxxname} ${_flagstr}\n${_err}")
        endif()
        execute_process(COMMAND ${_exe} 4096
                        RESULT_VARIABLE _rrc
                        OUTPUT_VARIABLE _hash
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _rrc EQUAL 0)
            message(FATAL_ERROR "run failed: ${_cxxname} ${_flagstr}")
        endif()
        message(STATUS "${_cxxname} ${_flagstr}: ${_hash}")
        list(APPEND _hashes "${_hash}")
        list(APPEND _labels "${_cxxname} ${_flagstr}")
    endforeach()
endforeach()

list(LENGTH _hashes _n)
if(_n LESS 2)
    message(FATAL_ERROR "determinism needs at least two build variants, got ${_n}")
endif()

list(GET _hashes 0 _first)
set(_i 0)
foreach(h IN LISTS _hashes)
    if(NOT h STREQUAL _first)
        list(GET _labels ${_i} _lab)
        message(FATAL_ERROR "DETERMINISM BROKEN: ${_lab} gave ${h}, expected ${_first}")
    endif()
    math(EXPR _i "${_i}+1")
endforeach()

message(STATUS "determinism: ${_n} build variants, one hash: ${_first}")
