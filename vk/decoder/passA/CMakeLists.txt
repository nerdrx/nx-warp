# NX Warp decoder Pass A: interleaved rANS entropy decode, CPU model,
# test encoder and headless Vulkan harness.  See README.md.

find_program(NXVW_PASSA_GLSLC glslc
  HINTS ${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}
        ${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64
        $ENV{ANDROID_NDK_HOME}/shader-tools/linux-x86_64
        $ENV{VULKAN_SDK}/bin
  PATHS /usr/bin /usr/local/bin)
if(NOT NXVW_PASSA_GLSLC)
  message(FATAL_ERROR "glslc not found (needed for vk/decoder/passA)")
endif()

# The tree's one shader-build rule and the one Adreno-safe pass list; see
# vk/common/cmake/NxvcShaderPasses.cmake for why `glslc -O` is never used.
set(NXVW_PASSA_GEN_SPV
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/nxvc_gen_spv.cmake)
set(NXVW_PASSA_SPV_PASSES_CMAKE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/NxvcShaderPasses.cmake)

set(NXVW_PASSA_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(NXVW_PASSA_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${NXVW_PASSA_GEN})

# The workgroup shape, as one number: tiles per group, eight lanes each, so the
# workgroup is eight times this many threads.  It reaches both the GLSL and the
# C++ that has to agree with it (nxs_tiles_per_group), so it is a cache
# variable rather than two independent -D flags.  See syntax_constants.h.
set(NXVW_PASSA_TPG "32" CACHE STRING
    "Pass A tiles per workgroup (workgroup = 8 * this threads)")
# PARENT_SCOPE so vk/decoder's own library, which includes syntax_constants.h
# for nxs_tiles_per_group(), compiles against the same number.
set(NXVW_PASSA_TPG ${NXVW_PASSA_TPG} PARENT_SCOPE)
add_compile_definitions(NXVW_PASSA_TILES_PER_GROUP=${NXVW_PASSA_TPG})

# ------------------------------------------------------------------- shader
set(NXVW_PASSA_SPV ${NXVW_PASSA_GEN}/rans_decode.spv.h)
add_custom_command(
  OUTPUT ${NXVW_PASSA_SPV}
  COMMAND ${CMAKE_COMMAND}
          -DGLSLC=${NXVW_PASSA_GLSLC}
          -DSRC=${NXVW_PASSA_DIR}/rans_decode.comp
          -DOUT=${NXVW_PASSA_SPV}
          -DNAME=rans_decode
          -DINCDIR=${NXVW_PASSA_DIR}
          -DDEFS=-DNXVW_PASSA_TILES_PER_GROUP=${NXVW_PASSA_TPG}
          -DSTYLE=plain
          -P ${NXVW_PASSA_GEN_SPV}
  DEPENDS ${NXVW_PASSA_DIR}/rans_decode.comp
          ${NXVW_PASSA_DIR}/syntax_constants.h
          ${NXVW_PASSA_GEN_SPV} ${NXVW_PASSA_SPV_PASSES_CMAKE}
  COMMENT "glslc rans_decode.comp -> rans_decode.spv.h"
  VERBATIM)
add_custom_target(nxvw_passA_spv DEPENDS ${NXVW_PASSA_SPV})

# ---------------------------------------------------------------- CPU model
add_library(nxvw_passA_model STATIC passA_model.cpp)
target_include_directories(nxvw_passA_model PUBLIC ${NXVW_PASSA_DIR})
set_target_properties(nxvw_passA_model PROPERTIES CXX_STANDARD 20
                                                  CXX_STANDARD_REQUIRED ON)

# Test-only rANS encoder + corpus generator.  Not part of any shipping target.
add_library(nxvw_passA_testkit STATIC passA_test_encoder.cpp)
target_link_libraries(nxvw_passA_testkit PUBLIC nxvw_passA_model)
set_target_properties(nxvw_passA_testkit PROPERTIES CXX_STANDARD 20
                                                    CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  foreach(_t nxvw_passA_model nxvw_passA_testkit)
    target_compile_options(${_t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endforeach()
endif()

# ------------------------------------------------------------------ harness
# Header/loader discovery follows vk/common: find_package first, then the
# project-wide -DNXVC_VK_HEADERS_DIR pointing at a Vulkan-Headers checkout.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
  find_path(NXVW_PASSA_VK_INCLUDE vulkan/vulkan.h
    HINTS ${NXVC_VK_HEADERS_DIR} $ENV{VULKAN_SDK}/include
    PATHS /usr/include /usr/local/include)
  find_library(NXVW_PASSA_VK_LIB NAMES vulkan vulkan-1
    HINTS $ENV{VULKAN_SDK}/lib PATHS /usr/lib /usr/lib64 /usr/local/lib)
endif()

if(Vulkan_FOUND OR (NXVW_PASSA_VK_INCLUDE AND NXVW_PASSA_VK_LIB))
  add_executable(nxvc-passA-test tools/nxvc-passA-test/main.cpp)
  add_dependencies(nxvc-passA-test nxvw_passA_spv)
  target_include_directories(nxvc-passA-test PRIVATE ${NXVW_PASSA_GEN})
  target_link_libraries(nxvc-passA-test PRIVATE nxvw_passA_testkit)
  if(Vulkan_FOUND)
    target_link_libraries(nxvc-passA-test PRIVATE Vulkan::Vulkan)
  else()
    target_include_directories(nxvc-passA-test PRIVATE ${NXVW_PASSA_VK_INCLUDE})
    target_link_libraries(nxvc-passA-test PRIVATE ${NXVW_PASSA_VK_LIB})
  endif()
  set_target_properties(nxvc-passA-test PROPERTIES CXX_STANDARD 20
                                                   CXX_STANDARD_REQUIRED ON)
  if(NOT MSVC)
    target_compile_options(nxvc-passA-test PRIVATE -Wall -Wextra
                           -Wno-unused-parameter -Wno-missing-field-initializers)
  endif()
  set(NXVW_PASSA_HAVE_VULKAN ON CACHE INTERNAL "")
else()
  message(WARNING "vk/decoder/passA: Vulkan loader/headers not found; "
                  "building the CPU model only")
  set(NXVW_PASSA_HAVE_VULKAN OFF CACHE INTERNAL "")
endif()
