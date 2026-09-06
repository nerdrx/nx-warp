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
