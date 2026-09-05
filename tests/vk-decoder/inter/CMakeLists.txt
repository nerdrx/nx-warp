# vk.passW.* -- the inter predictor's GPU-vs-CPU harness.
#
# The binary lives with the kernel (vk/decoder/inter/tools/) exactly as the
# Pass A and Pass B harnesses do; this file only registers it.

if(NOT TARGET nxvc-passW-test)
  return()
endif()

add_test(NAME vk.passW.gpu_vs_cpu COMMAND nxvc-passW-test --verbose)
set_tests_properties(vk.passW.gpu_vs_cpu PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 900)

add_test(NAME vk.passW.gpu_vs_cpu_radv COMMAND nxvc-passW-test)
set_tests_properties(vk.passW.gpu_vs_cpu_radv PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 900
  ENVIRONMENT "NXVC_VKD_DEVICE=RADV")

add_test(NAME vk.passW.gpu_vs_cpu_lavapipe COMMAND nxvc-passW-test)
set_tests_properties(vk.passW.gpu_vs_cpu_lavapipe PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 1800)
if(NXVC_LAVAPIPE_ICD AND EXISTS "${NXVC_LAVAPIPE_ICD}")
  set_tests_properties(vk.passW.gpu_vs_cpu_lavapipe PROPERTIES
    ENVIRONMENT "VK_DRIVER_FILES=${NXVC_LAVAPIPE_ICD};VK_ICD_FILENAMES=${NXVC_LAVAPIPE_ICD};NXVC_VKD_DEVICE=llvmpipe")
endif()
