# Pass A tests.  Every GPU test exits 77 when no usable ICD is present, so a
# machine without Vulkan reports SKIP rather than failure.

set(NXVW_PASSA_SRC ${CMAKE_SOURCE_DIR}/vk/decoder/passA)

# --------------------------------------------------------- CPU round trip
add_executable(test_passA_model_roundtrip test_model_roundtrip.cpp)
target_link_libraries(test_passA_model_roundtrip PRIVATE nxvw_passA_testkit)
set_target_properties(test_passA_model_roundtrip
                      PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME vk.passA.model_roundtrip COMMAND test_passA_model_roundtrip)

# ------------------------------------------- conformance against ref/ (CPU)
if(TARGET nxvc_ref AND EXISTS ${CMAKE_SOURCE_DIR}/ref/src/entropy.h)
  add_executable(test_passA_ref_agreement test_ref_agreement.cpp)
  target_link_libraries(test_passA_ref_agreement PRIVATE nxvw_passA_testkit
                                                         nxvc_ref)
  target_include_directories(test_passA_ref_agreement
                             PRIVATE ${CMAKE_SOURCE_DIR}/ref/src)
  target_compile_definitions(test_passA_ref_agreement PRIVATE
    NXVW_PASSA_VECTOR_DIR="${CMAKE_SOURCE_DIR}/tests/vectors")
  set_target_properties(test_passA_ref_agreement
                        PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  add_test(NAME vk.passA.ref_agreement COMMAND test_passA_ref_agreement)
endif()

# ------------------------------------------------------------- GPU tests
if(NXVW_PASSA_HAVE_VULKAN)
  # The headline gate: 2048 tiles at ~0.5 symbols/pixel, zero mismatches.
  add_test(NAME vk.passA.gpu_roundtrip
           COMMAND nxvc-passA-test --tiles 2048 --iters 4)

  # The two read-pointer modes separately, so a failure names the path.
  add_test(NAME vk.passA.gpu_ballot
           COMMAND nxvc-passA-test --tiles 512 --iters 1 --mode ballot)
  add_test(NAME vk.passA.gpu_lds_fallback
           COMMAND nxvc-passA-test --tiles 512 --iters 1 --mode lds)

  # Wave32 and wave64 put the 8-lane clusters in different subgroups.
  add_test(NAME vk.passA.gpu_subgroup32
           COMMAND nxvc-passA-test --tiles 256 --iters 1 --subgroup 32)
  add_test(NAME vk.passA.gpu_subgroup64
           COMMAND nxvc-passA-test --tiles 256 --iters 1 --subgroup 64)

  # [entropy-lite] ENTROPY_LITE / FIXED: the same corpus and the same
  # GPU-vs-model gate, decoded by the table-free tool instead.  The read
  # pointer has no meaning there, so one --mode is enough; --intra adds the
  # mode units, which only the Lite encoder can code.
  add_test(NAME vk.passA.gpu_lite
           COMMAND nxvc-passA-test --tiles 2048 --iters 4 --entropy lite
                   --mode ballot)
  add_test(NAME vk.passA.gpu_lite_intra
           COMMAND nxvc-passA-test --tiles 512 --iters 1 --entropy lite
                   --mode ballot --intra)
  add_test(NAME vk.passA.gpu_lite_subgroup32
           COMMAND nxvc-passA-test --tiles 256 --iters 1 --entropy lite
                   --mode ballot --subgroup 32)
  add_test(NAME vk.passA.gpu_lite_subgroup64
           COMMAND nxvc-passA-test --tiles 256 --iters 1 --entropy lite
                   --mode ballot --subgroup 64)

  foreach(_t vk.passA.gpu_roundtrip vk.passA.gpu_ballot
             vk.passA.gpu_lds_fallback vk.passA.gpu_subgroup32
             vk.passA.gpu_subgroup64 vk.passA.gpu_lite
             vk.passA.gpu_lite_intra vk.passA.gpu_lite_subgroup32
             vk.passA.gpu_lite_subgroup64)
    set_tests_properties(${_t} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
  endforeach()
endif()
