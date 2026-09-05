# vk.decoder.unorm_roundtrip -- the exactness proof behind Pass B's UNORM
# store (reconstruct.comp specialization constant 5).  Deliberately standalone:
# it links no decoder code, so a driver can be disqualified from the UNORM path
# without running the conformance sweep, and so the answer for a driver is
# about the driver rather than about this codec.
#
# Skips (exit 77) wherever glslc or a Vulkan loader is missing.

find_program(NXVW_UNORM_GLSLC glslc
  HINTS ${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}
        ${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64
        $ENV{ANDROID_NDK_HOME}/shader-tools/linux-x86_64
        $ENV{VULKAN_SDK}/bin
  PATHS /usr/bin /usr/local/bin)
# The same discovery order as vk/decoder and the two pass harnesses:
# find_package first, then the project-wide -DNXVC_VK_HEADERS_DIR.  Several of
# the machines this builds on have a loader but no system headers, so
# find_package(Vulkan) alone is not enough.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
  find_path(NXVW_UNORM_VK_INCLUDE vulkan/vulkan.h
    HINTS ${NXVC_VK_HEADERS_DIR} ${NXVW_VULKAN_HEADERS}
          $ENV{VULKAN_SDK}/include
          ${CMAKE_SOURCE_DIR}/../tools/local/include
    PATHS /usr/include /usr/local/include)
  find_library(NXVW_UNORM_VK_LIB NAMES vulkan vulkan-1
    HINTS $ENV{VULKAN_SDK}/lib PATHS /usr/lib /usr/lib64 /usr/local/lib)
endif()
if(NOT NXVW_UNORM_GLSLC OR
   NOT (Vulkan_FOUND OR (NXVW_UNORM_VK_INCLUDE AND NXVW_UNORM_VK_LIB)))
  message(STATUS "tests/vk-decoder/unorm: no glslc or no Vulkan, skipped")
  return()
endif()

set(_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${_gen})
set(_spv ${_gen}/unorm_roundtrip.spv.h)

# The same Adreno-safe pass list the two kernels take (bench/README.md,
# "Adreno and spirv-opt"): a test that is compiled differently from the code
# it vouches for is not a test of that code's toolchain.
add_custom_command(
  OUTPUT ${_spv}
  COMMAND ${CMAKE_COMMAND}
          -DGLSLC=${NXVW_UNORM_GLSLC}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/unorm_roundtrip.comp
          -DOUT=${_spv}
          -DNAME=unorm_roundtrip
          -DINCDIR=${CMAKE_CURRENT_SOURCE_DIR}
          -P ${CMAKE_SOURCE_DIR}/vk/decoder/passB/cmake/gen_spv.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unorm_roundtrip.comp
          ${CMAKE_SOURCE_DIR}/vk/decoder/passB/cmake/gen_spv.cmake
  COMMENT "glslc unorm_roundtrip.comp -> unorm_roundtrip.spv.h"
  VERBATIM)

add_executable(test_unorm_roundtrip test_unorm_roundtrip.cpp ${_spv})
target_include_directories(test_unorm_roundtrip PRIVATE ${_gen})
if(Vulkan_FOUND)
  target_link_libraries(test_unorm_roundtrip PRIVATE Vulkan::Vulkan)
else()
  target_include_directories(test_unorm_roundtrip PRIVATE
                             ${NXVW_UNORM_VK_INCLUDE})
  target_link_libraries(test_unorm_roundtrip PRIVATE ${NXVW_UNORM_VK_LIB})
endif()
set_target_properties(test_unorm_roundtrip PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(test_unorm_roundtrip PRIVATE -Wall -Wextra
                         -Wno-unused-parameter
                         -Wno-missing-field-initializers)
endif()

add_test(NAME vk.decoder.unorm_roundtrip
         COMMAND test_unorm_roundtrip --verbose)
set_tests_properties(vk.decoder.unorm_roundtrip PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 300)

add_test(NAME vk.decoder.unorm_roundtrip_radv
         COMMAND test_unorm_roundtrip --verbose)
set_tests_properties(vk.decoder.unorm_roundtrip_radv PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 300 ENVIRONMENT "NXVC_VKD_DEVICE=RADV")

add_test(NAME vk.decoder.unorm_roundtrip_lavapipe
         COMMAND test_unorm_roundtrip --verbose)
set_tests_properties(vk.decoder.unorm_roundtrip_lavapipe PROPERTIES
  SKIP_RETURN_CODE 77 TIMEOUT 300)
if(NXVC_LAVAPIPE_ICD AND EXISTS "${NXVC_LAVAPIPE_ICD}")
  set_tests_properties(vk.decoder.unorm_roundtrip_lavapipe PROPERTIES
    ENVIRONMENT "VK_DRIVER_FILES=${NXVC_LAVAPIPE_ICD};VK_ICD_FILENAMES=${NXVC_LAVAPIPE_ICD};NXVC_VKD_DEVICE=llvmpipe")
endif()
