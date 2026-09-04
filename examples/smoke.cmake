# smoke.cmake -- drive the examples end to end from ctest.
#
# Invoked as `cmake -DENC=... -DDEC=... -DRT=... -DTW=... -DWORK=... -P smoke.cmake`.
# No Python, no ffmpeg, no test framework: this has to run anywhere the codec
# builds, including a bare CI container.
#
# The input is a repeating printable-ASCII pattern reinterpreted as 8-bit YUV.
# That is not a picture, but it is legal input with plenty of high-frequency
# content, and CMake can write it without a binary-file primitive.  The point of
# this test is that the examples run and the round trip closes, not that the
# material is representative -- corpus/ is where representative material lives.

foreach(v ENC DEC RT TW WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "smoke.cmake: -D${v} is required")
  endif()
endforeach()

set(W 128)
set(H 128)
math(EXPR NEED "${W} * ${H} * 3 * 2")   # 4:4:4, three planes, two frames

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(YUV "${WORK}/in.yuv")

# 71 characters, coprime with the 128-pixel row length, so the pattern does not
# line up with the tile grid and every tile differs from its neighbours.
set(_unit "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*")
string(LENGTH "${_unit}" _ulen)
math(EXPR _reps "${NEED} / ${_ulen} + 1")
string(REPEAT "${_unit}" ${_reps} _pat)
string(SUBSTRING "${_pat}" 0 ${NEED} _data)
file(WRITE "${YUV}" "${_data}")

file(SIZE "${YUV}" _sz)
if(NOT _sz EQUAL ${NEED})
  message(FATAL_ERROR "smoke: wrote ${_sz} bytes, wanted ${NEED}")
endif()

function(run_step label)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc
                  OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "smoke: ${label} failed (rc=${_rc})\n${_out}\n${_err}")
  endif()
  message(STATUS "smoke: ${label}\n${_out}")
endfunction()

run_step("encode" "${ENC}" --in "${YUV}" --w ${W} --h ${H} --pix yuv444p
         --qp 24 --frames 2 --out "${WORK}/out.nxv")
run_step("decode" "${DEC}" --in "${WORK}/out.nxv" --out "${WORK}/out.yuv")
run_step("tilewalk" "${TW}" --in "${WORK}/out.nxv" --frame 0 --no-table
         --csv "${WORK}/tiles.csv")
run_step("roundtrip lossy" "${RT}" --in "${YUV}" --w ${W} --h ${H}
         --pix yuv444p --qp 24 --frames 2)

# The strongest single assertion here: lossless must reproduce the input
# exactly.  roundtrip_psnr exits non-zero if any plane's PSNR is finite under
# --lossless, so no output parsing is needed.
run_step("roundtrip lossless" "${RT}" --in "${YUV}" --w ${W} --h ${H}
         --pix yuv444p --lossless --frames 2)

file(SIZE "${WORK}/out.yuv" _decsz)
if(NOT _decsz EQUAL ${NEED})
  message(FATAL_ERROR "smoke: decoded ${_decsz} bytes, expected ${NEED}")
endif()

file(READ "${WORK}/tiles.csv" _csv)
string(FIND "${_csv}" "INTRA" _has_intra)
if(_has_intra EQUAL -1)
  message(FATAL_ERROR "smoke: tile CSV has no INTRA tiles; a Phase 1 stream must be all intra")
endif()

message(STATUS "smoke: OK")
