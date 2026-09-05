# vk.decoder.* -- end-to-end conformance and timing for nxvc_vk_decoder.
#
# Everything skips (exit 77) rather than failing when the piece it needs is
# absent, so this directory is safe to configure anywhere.

if(NOT TARGET nxvc_vk_decoder OR NOT TARGET nxvc_ref)
  return()
endif()

add_executable(test_vk_decoder_conformance test_vk_decoder_conformance.cpp)
target_link_libraries(test_vk_decoder_conformance
                      PRIVATE nxvc_vk_decoder nxvc_ref)
target_include_directories(test_vk_decoder_conformance PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/tests/ref)   # test_util.h: MD5 and the test images
target_compile_definitions(test_vk_decoder_conformance PRIVATE
  NXVC_VECTORS_DIR="${CMAKE_SOURCE_DIR}/tests/vectors")
set_target_properties(test_vk_decoder_conformance PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(test_vk_decoder_conformance PRIVATE -Wall -Wextra
                         -Wno-unused-parameter)
endif()

# The exit criterion: every frozen vector and every synthetic stream decodes
# on the GPU with zero mismatching samples against ref/, on the default ICD.
add_test(NAME vk.decoder.conformance
         COMMAND test_vk_decoder_conformance --verbose)
set_tests_properties(vk.decoder.conformance PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 1800)

# The same sweep pinned to each ICD we ship against.  --device only ever
# selects a matching device and never falls back, so these exit 77 when the
# named driver is not present.
add_test(NAME vk.decoder.conformance_radv
         COMMAND test_vk_decoder_conformance --verbose)
set_tests_properties(vk.decoder.conformance_radv PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 1800
  ENVIRONMENT "NXVC_VKD_DEVICE=RADV")

add_test(NAME vk.decoder.conformance_lavapipe
         COMMAND test_vk_decoder_conformance --verbose)
set_tests_properties(vk.decoder.conformance_lavapipe PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 7200)
if(NXVC_LAVAPIPE_ICD AND EXISTS "${NXVC_LAVAPIPE_ICD}")
  set_tests_properties(vk.decoder.conformance_lavapipe PROPERTIES
    ENVIRONMENT "VK_DRIVER_FILES=${NXVC_LAVAPIPE_ICD};VK_ICD_FILENAMES=${NXVC_LAVAPIPE_ICD};NXVC_VKD_DEVICE=llvmpipe")
endif()

# [inter] The loss test on its own, as a named entry: a 100-frame inter
# sequence with random tiles marked missing through
# nxvc_vk_decoder_mark_missing(), decoded beside the reference fed the same
# drops through nxvc_decoder_set_lost_tiles(), compared byte for byte.
# docs/TRANSPORT.md 8 and docs/SYNTAX.md 13.6.  It is inside
# vk.decoder.conformance as well; this entry is what a bisect points at.
add_test(NAME vk.decoder.loss
         COMMAND test_vk_decoder_conformance --only-loss)
set_tests_properties(vk.decoder.loss PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 1800)

# [inter] Informational: a 36-frame inter sequence, which is the shape whose
# cost the intra bench cannot express -- the mode mix is a property of where
# the sequence is, not of one frame.
add_test(NAME vk.decoder.bench_inter
         COMMAND test_vk_decoder_conformance --bench 4 --bench-inter 36
                 --bench-size 1024 1024 --bench-qp 24)
set_tests_properties(vk.decoder.bench_inter PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 1800)

# Informational: full-frame decode time for two 2048x2048 eyes at 4:2:0.
add_test(NAME vk.decoder.bench COMMAND test_vk_decoder_conformance --bench 20)
set_tests_properties(vk.decoder.bench PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 1800)

# The CLI has to behave like nxv-dec for the quality harness' --codec-cmd
# contract: same options, same planar output.
if(TARGET nxvc-vkdec AND TARGET nxv-dec)
  add_test(NAME vk.decoder.cli
           COMMAND ${CMAKE_COMMAND}
                   -DVKDEC=$<TARGET_FILE:nxvc-vkdec>
                   -DREFDEC=$<TARGET_FILE:nxv-dec>
                   -DVECTORS=${CMAKE_SOURCE_DIR}/tests/vectors
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_compare.cmake)
  # `cmake -P` cannot pick its own exit code, so the skip is a printed marker.
  set_tests_properties(vk.decoder.cli PROPERTIES
    SKIP_REGULAR_EXPRESSION "no usable Vulkan ICD" TIMEOUT 900)
endif()
