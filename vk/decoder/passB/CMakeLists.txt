# NX Warp decoder Pass B: reconstruction kernel, CPU model and test harness.

find_program(NXVW_GLSLC glslc
  HINTS ${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}
        ${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64
        $ENV{ANDROID_NDK_HOME}/shader-tools/linux-x86_64
        $ENV{VULKAN_SDK}/bin
  PATHS /usr/bin /usr/local/bin)
if(NOT NXVW_GLSLC)
  message(FATAL_ERROR "glslc not found (needed for vk/decoder/passB)")
endif()

set(NXVW_PASSB_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(NXVW_PASSB_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${NXVW_PASSB_GEN})

set(NXVW_PASSB_SPV ${NXVW_PASSB_GEN}/reconstruct.spv.h)
add_custom_command(
  OUTPUT ${NXVW_PASSB_SPV}
  COMMAND ${CMAKE_COMMAND}
          -DGLSLC=${NXVW_GLSLC}
          -DSRC=${NXVW_PASSB_DIR}/reconstruct.comp
          -DOUT=${NXVW_PASSB_SPV}
          -DNAME=reconstruct
          -DINCDIR=${NXVW_PASSB_DIR}
          -P ${NXVW_PASSB_DIR}/cmake/gen_spv.cmake
  DEPENDS ${NXVW_PASSB_DIR}/reconstruct.comp
          ${NXVW_PASSB_DIR}/syntax_constants.h
          ${NXVW_PASSB_DIR}/passB_layout.h
          ${NXVW_PASSB_DIR}/cmake/gen_spv.cmake
  COMMENT "glslc reconstruct.comp -> reconstruct.spv.h"
  VERBATIM)
add_custom_target(nxvw_passB_spv DEPENDS ${NXVW_PASSB_SPV})

# ---------------------------------------------------------------- CPU model
add_library(nxvw_passB_model STATIC passB_model.cpp)
target_include_directories(nxvw_passB_model PUBLIC ${NXVW_PASSB_DIR})
set_target_properties(nxvw_passB_model PROPERTIES CXX_STANDARD 20
                                                  CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(nxvw_passB_model PRIVATE -Wall -Wextra
                         -Wno-unused-parameter)
endif()

# ------------------------------------------------------------------ harness
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
  find_path(NXVW_VK_INCLUDE vulkan/vulkan.h
    HINTS ${NXVW_VULKAN_HEADERS}
          $ENV{VULKAN_SDK}/include
          ${CMAKE_SOURCE_DIR}/../tools/vulkan-sdk/include
          ${CMAKE_SOURCE_DIR}/../tools/vulkan-headers-src/include
          ${CMAKE_SOURCE_DIR}/../tools/local/include
          ${CMAKE_SOURCE_DIR}/../tools/Vulkan-Headers-1.4.309/include
    PATHS /usr/include /usr/local/include)
  find_library(NXVW_VK_LIB NAMES vulkan vulkan-1
    HINTS $ENV{VULKAN_SDK}/lib PATHS /usr/lib /usr/lib64 /usr/local/lib)
endif()

if(Vulkan_FOUND OR (NXVW_VK_INCLUDE AND NXVW_VK_LIB))
  add_executable(nxvc-passB-test tools/nxvc-passB-test.cpp)
  add_dependencies(nxvc-passB-test nxvw_passB_spv)
  target_include_directories(nxvc-passB-test PRIVATE ${NXVW_PASSB_GEN})
  target_link_libraries(nxvc-passB-test PRIVATE nxvw_passB_model)
  if(Vulkan_FOUND)
    target_link_libraries(nxvc-passB-test PRIVATE Vulkan::Vulkan)
  else()
    target_include_directories(nxvc-passB-test PRIVATE ${NXVW_VK_INCLUDE})
    target_link_libraries(nxvc-passB-test PRIVATE ${NXVW_VK_LIB})
  endif()
  set_target_properties(nxvc-passB-test PROPERTIES CXX_STANDARD 20
                                                   CXX_STANDARD_REQUIRED ON)
  if(NOT MSVC)
    target_compile_options(nxvc-passB-test PRIVATE -Wall -Wextra
                           -Wno-unused-parameter -Wno-missing-field-initializers)
  endif()
  set(NXVW_PASSB_HAVE_VULKAN ON CACHE INTERNAL "")
else()
  message(WARNING "vk/decoder/passB: Vulkan loader/headers not found; "
                  "building the CPU model only")
  set(NXVW_PASSB_HAVE_VULKAN OFF CACHE INTERNAL "")
endif()
