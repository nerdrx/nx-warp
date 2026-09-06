# The atlas patch layout the C ABI reports, checked against the copies that
# consume it.
#
# `nxvc_vk_encoder_atlas_write_tiles()` takes a buffer "already in the atlas
# layout", and a caller has to build that buffer.  Before the accessor existed
# the only way to do so was to reproduce nxvw_ring_layout() out of the
# DECODER's private inter_layout.h -- a second copy of the plane offsets, the
# even-padded row stride and the per-eye column origin.  That copy is the
# dangerous kind of duplicate: it passes every check the encoder makes (the
# copy sizes are right, the call returns OK, the stream is well formed) and
# still writes the pixels to the wrong addresses, so the failure surfaces as a
# wrong picture frames later and nowhere near the layout.
#
# So the encoder reports the layout, and this test pins that the report is
# true: --atlas-layout-selftest builds a slot-shaped patch image from the
# accessor's numbers ALONE, copies a checkerboard of tiles through the
# encoder's own region builder, and verifies every sample of every plane
# arrived where the accessor said it would.  A checkerboard because every
# patched tile then borders unpatched ones, so an off-by-one in a stride or an
# eye origin bleeds into a neighbour instead of being invisibly self-
# consistent.
#
# Expects VKENC, WORKDIR, DEVICE.

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

# Both chroma formats, because the tile extent and the plane heights differ
# between them -- 32-sample chroma tiles and half-height planes under 4:2:0,
# 64 and full height under 4:4:4 -- and those are exactly the terms a
# hand-written second copy of the layout gets wrong.
#
# The eye pair is NOT covered here for the reason atlas_acid.cmake gives: the
# fixture dumper writes a mono picture and CMake script mode cannot synthesise
# a side-by-side one.  It is covered by hand on the stereo fixture, and the
# per-eye column origin it exercises is the `eye_stride` field the accessor
# reports rather than a separate code path.
foreach(pix yuv420p yuv444p)
  execute_process(COMMAND ${VKENC} --in ${YUV} --w ${W} --h ${H} --pix ${pix}
                          --qp 26 --frames ${FRAMES} --poses ${POSES}
                          --intra-period 6 --inter --atlas --device ${DEVICE}
                          --out ${WORKDIR}/unused.nxv
                          --atlas-layout-selftest
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE eout)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "the reported atlas layout does not address the samples the encoder's "
      "own copies do (${pix}): ${out}${eout}")
  endif()
  string(STRIP "${out}" out)
  message(STATUS "vk.encoder.atlas.layout ${pix}: ${out}")
endforeach()
