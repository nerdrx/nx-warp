# [ATLAS] 13.12.2's composition against an independent 128-bit oracle.  Links
# no Vulkan and needs no device, so it runs everywhere the project builds.
if(NOT TARGET nxvw_atlas_model)
  return()
endif()
add_executable(nxvc-atlas-compose-test test_atlas_compose.cpp)
target_link_libraries(nxvc-atlas-compose-test PRIVATE nxvw_atlas_model)
set_target_properties(nxvc-atlas-compose-test PROPERTIES CXX_STANDARD 20
                                                         CXX_STANDARD_REQUIRED ON)
add_test(NAME vk.atlas.compose COMMAND nxvc-atlas-compose-test)
set_tests_properties(vk.atlas.compose PROPERTIES TIMEOUT 300)

# The host bookkeeping: monotonicity, the lazy-advance selection, the H ring
# window.  No Vulkan and no device.
add_executable(nxvc-atlas-state-test test_atlas_state.cpp)
target_link_libraries(nxvc-atlas-state-test PRIVATE nxvw_atlas_model)
set_target_properties(nxvc-atlas-state-test PROPERTIES CXX_STANDARD 20
                                                       CXX_STANDARD_REQUIRED ON)
add_test(NAME vk.atlas.state COMMAND nxvc-atlas-state-test)
set_tests_properties(vk.atlas.state PROPERTIES TIMEOUT 300)

# The decoder's model against the REFERENCE CODEC's, in one binary.  Everything
# downstream -- the GPU kernel, and conformance itself -- inherits whatever the
# model gets wrong, so "pinned to ref/src/inter.h" has to be a test and not a
# comment.  Needs nxvc_ref, so it is guarded on it.
if(TARGET nxvc_ref)
  add_executable(nxvc-atlas-vs-ref-test test_atlas_vs_ref.cpp)
  target_link_libraries(nxvc-atlas-vs-ref-test PRIVATE nxvw_atlas_model nxvc_ref)
  target_include_directories(nxvc-atlas-vs-ref-test PRIVATE
                             ${CMAKE_SOURCE_DIR}/include
                             ${CMAKE_SOURCE_DIR}/ref/src)
  set_target_properties(nxvc-atlas-vs-ref-test PROPERTIES CXX_STANDARD 20
                                                          CXX_STANDARD_REQUIRED ON)
  add_test(NAME vk.atlas.vs_ref COMMAND nxvc-atlas-vs-ref-test)
  set_tests_properties(vk.atlas.vs_ref PROPERTIES TIMEOUT 300)
endif()

# ---------------------------------------------------------------------------
# vk.atlas.gpu_vs_cpu -- atlas_compose.comp against atlas_model.cpp, and the
# lazy advance against the eager one.  Both desktop ICDs, because a subgroup
# scan in Pass A once validated byte-exactly on RADV AND lavapipe and silently
# miscomputed on the Adreno: two ICDs agreeing is not evidence about the target
# part, which is why the device leg is mandatory and lives in run-android.sh.
if(NOT TARGET nxvc-atlas-gpu-test)
  return()
endif()

add_test(NAME vk.atlas.gpu_vs_cpu COMMAND nxvc-atlas-gpu-test --verbose)
# --bench is not registered as a test: a timing number is not a pass/fail, and
# it walks the frame number past the correctness run, which is why the bench
# block runs AFTER the flush comparison and not before it.
set_tests_properties(vk.atlas.gpu_vs_cpu PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 900)

add_test(NAME vk.atlas.gpu_vs_cpu_radv COMMAND nxvc-atlas-gpu-test)
set_tests_properties(vk.atlas.gpu_vs_cpu_radv PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 900
  ENVIRONMENT "NXVC_VKD_DEVICE=RADV")

add_test(NAME vk.atlas.gpu_vs_cpu_lavapipe COMMAND nxvc-atlas-gpu-test)
set_tests_properties(vk.atlas.gpu_vs_cpu_lavapipe PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 1800)
if(NXVC_LAVAPIPE_ICD AND EXISTS "${NXVC_LAVAPIPE_ICD}")
  set_tests_properties(vk.atlas.gpu_vs_cpu_lavapipe PROPERTIES
    ENVIRONMENT "VK_DRIVER_FILES=${NXVC_LAVAPIPE_ICD};VK_ICD_FILENAMES=${NXVC_LAVAPIPE_ICD};NXVC_VKD_DEVICE=llvmpipe")
endif()

if(TARGET nxvc_vk_decoder)
  add_executable(nxvc-atlas-borrowed-r8-test test_borrowed_r8.cpp)
  target_link_libraries(nxvc-atlas-borrowed-r8-test PRIVATE nxvc_vk_decoder)
  target_include_directories(nxvc-atlas-borrowed-r8-test PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(nxvc-atlas-borrowed-r8-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  add_test(NAME vk.decoder.atlas_borrowed_r8 COMMAND nxvc-atlas-borrowed-r8-test)
  set_tests_properties(vk.decoder.atlas_borrowed_r8 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
