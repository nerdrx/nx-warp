# vk.encoder.passw.same -- the encoder's Pass W and Pass B are the decoder's.
#
# The single most important rule in the project is that the encoder must never
# hold a reference the decoder cannot reproduce (vk/encoder/README.md, the E3b
# row).  For the pose-warp predictor the guarantee is meant to be structural:
# the encoder does not have a warp shader, it compiles the DECODER's
# `warp_pred.comp` from the decoder's own source tree, so the two are the same
# SPIR-V by construction rather than by two files agreeing.
#
# "By construction" is exactly the kind of claim this repository has already
# been bitten by twice -- `nxe_enc.h`'s `pred_src` buffer that never existed,
# and the static assertions that could not see a GLSL define.  So it is checked
# rather than asserted: this test extracts the SPIR-V words from both generated
# headers and requires them to be identical, word for word.
#
# It needs no GPU and no device.  If it ever fails, someone has given the
# encoder its own copy of the predictor, or the two builds have diverged in
# their glslc flags, spirv-opt pass list or include path -- and the encoder's
# reference picture has quietly stopped being the decoder's.
#
# Expects DEC and ENC to name the two generated .spv.h files.

if(NOT EXISTS "${DEC}")
  message(FATAL_ERROR "vk.encoder.passw.same: no decoder module at ${DEC}")
endif()
if(NOT EXISTS "${ENC}")
  message(FATAL_ERROR "vk.encoder.passw.same: no encoder module at ${ENC}")
endif()

file(READ "${DEC}" _dec)
file(READ "${ENC}" _enc)

# The two headers differ in STYLE (the decoder emits `plain`, the encoder
# `guarded`) and therefore in their preamble and symbol name.  Only the module
# is being compared, so both are reduced to their hexadecimal words.
string(REGEX MATCHALL "0x[0-9a-fA-F]+" _dw "${_dec}")
string(REGEX MATCHALL "0x[0-9a-fA-F]+" _ew "${_enc}")

list(LENGTH _dw _dn)
list(LENGTH _ew _en)

if(_dn EQUAL 0)
  message(FATAL_ERROR "vk.encoder.passw.same: no SPIR-V words in ${DEC}")
endif()

if(NOT _dn EQUAL _en)
  message(FATAL_ERROR
    "vk.encoder.passw.same: the encoder's Pass W is ${_en} words and the "
    "decoder's is ${_dn}.  They are supposed to be the same module compiled "
    "once from vk/decoder/inter/warp_pred.comp.")
endif()

# Compared as one string rather than element by element: a CMake `foreach` over
# 42490 words takes a minute and a half for Pass B, and a test nobody wants to
# wait for is a test that gets excluded from the run.
string(REPLACE ";" "," _da "${_dw}")
string(REPLACE ";" "," _ea "${_ew}")
if(NOT _da STREQUAL _ea)
  # Only on failure is it worth paying for the position.
  math(EXPR _last "${_dn} - 1")
  foreach(i RANGE ${_last})
    list(GET _dw ${i} _a)
    list(GET _ew ${i} _b)
    if(NOT _a STREQUAL _b)
      message(FATAL_ERROR
        "vk.encoder.passw.same: the modules differ at word ${i} of ${_dn} "
        "(decoder ${_a}, encoder ${_b}).  The encoder's shader is no longer "
        "the decoder's, so its reference picture is no longer one the decoder "
        "can reproduce.")
    endif()
  endforeach()
  message(FATAL_ERROR "vk.encoder.passw.same: modules differ")
endif()

message(STATUS "vk.encoder.passw.same: ${_dn} words identical")
