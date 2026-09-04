# Pass B tests.  Everything skips (exit 77) rather than failing when the piece
# it needs is absent, so this directory is safe to configure anywhere.

# ------------------------------------------------- CPU model vs ref/ (no GPU)
if(TARGET nxvw_passB_model AND TARGET nxvc_ref)
  add_executable(test_passB_ref test_passB_ref.cpp)
  target_link_libraries(test_passB_ref PRIVATE nxvw_passB_model nxvc_ref)
  target_include_directories(test_passB_ref PRIVATE
    ${CMAKE_SOURCE_DIR}/ref/src ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(test_passB_ref PROPERTIES CXX_STANDARD 20
                                                  CXX_STANDARD_REQUIRED ON)
  add_test(NAME vk.passB.ref_conformance COMMAND test_passB_ref)
  set_tests_properties(vk.passB.ref_conformance PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# ------------------------------------------------------ CPU model vs GPU
if(TARGET nxvc-passB-test)
  # Full sweep on the default device, then the same sweep pinned to each ICD we
  # care about.  Every one of these exits 77 when no usable ICD is present.
  add_test(NAME vk.passB.gpu_roundtrip
           COMMAND nxvc-passB-test --verbose)
  set_tests_properties(vk.passB.gpu_roundtrip PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 1800)

  add_test(NAME vk.passB.gpu_roundtrip_radv
           COMMAND nxvc-passB-test --verbose --device-name RADV)
  set_tests_properties(vk.passB.gpu_roundtrip_radv PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 1800)

  # lavapipe: only when the caller points VK_DRIVER_FILES at an lvp ICD.
  add_test(NAME vk.passB.gpu_roundtrip_lavapipe
           COMMAND nxvc-passB-test --verbose --device-name llvmpipe)
  set_tests_properties(vk.passB.gpu_roundtrip_lavapipe PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 3600)

  # Informational: 2048-tile dispatch time and coefficient SSBO traffic.
  add_test(NAME vk.passB.bench COMMAND nxvc-passB-test --bench)
  set_tests_properties(vk.passB.bench PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
