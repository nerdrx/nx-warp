# NX Warp decoder, inter path (docs/SYNTAX.md 13).
#
#   warp_pred.comp     Pass W, the predictor kernel
#   inter_hook.glsl    Pass B's three-line half of it (compiled inside
#                      passB/reconstruct.comp, not on its own)
#   inter_layout.h     the one description of every buffer, shared by all four
#   inter_state.h      the host state: prediction history, ring, missing map
#   inter_model.{h,cpp}  the line-for-line CPU model of Pass W
#
# The kernel is built with the tree's one shader rule and the one Adreno-safe
# pass list, exactly as passA/ and passB/ are.

# The tree's one shader-build rule and its one Adreno-safe pass list.  They
# are named again here rather than inherited from passB/: a `set()` there is
# directory-scoped and a sibling directory does not see it.
set(NXVW_PASSB_GEN_SPV
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/nxvc_gen_spv.cmake)
set(NXVW_PASSB_SPV_PASSES_CMAKE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/NxvcShaderPasses.cmake)

set(NXVW_INTER_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(NXVW_INTER_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${NXVW_INTER_GEN})

set(NXVW_INTER_SPV ${NXVW_INTER_GEN}/warp_pred.spv.h)
add_custom_command(
  OUTPUT ${NXVW_INTER_SPV}
  COMMAND ${CMAKE_COMMAND}
          -DGLSLC=${NXVW_GLSLC}
          -DSRC=${NXVW_INTER_DIR}/warp_pred.comp
          -DOUT=${NXVW_INTER_SPV}
          -DNAME=warp_pred
          -DINCDIR=${NXVW_INTER_DIR}
          -DSTYLE=plain
          -P ${NXVW_PASSB_GEN_SPV}
  DEPENDS ${NXVW_INTER_DIR}/warp_pred.comp
          ${NXVW_INTER_DIR}/inter_layout.h
          ${NXVW_INTER_DIR}/../passB/syntax_constants.h
          ${NXVW_INTER_DIR}/../passB/passB_layout.h
          ${NXVW_PASSB_GEN_SPV} ${NXVW_PASSB_SPV_PASSES_CMAKE}
  COMMENT "glslc warp_pred.comp -> warp_pred.spv.h"
  VERBATIM)
add_custom_target(nxvw_inter_spv DEPENDS ${NXVW_INTER_SPV})

# ---------------------------------------------------------------- CPU model
add_library(nxvw_inter_model STATIC inter_model.cpp)
target_include_directories(nxvw_inter_model PUBLIC
                           ${NXVW_INTER_DIR} ${NXVW_INTER_DIR}/..
                           ${CMAKE_SOURCE_DIR}/warp/include)
target_link_libraries(nxvw_inter_model PUBLIC nxvc_warp_ref)
set_target_properties(nxvw_inter_model PROPERTIES CXX_STANDARD 20
                                                  CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(nxvw_inter_model PRIVATE -Wall -Wextra
                         -Wno-unused-parameter)
endif()

# ------------------------------------------------------------------ harness
# GPU-vs-CPU for Pass W, the same shape passA/ and passB/ already carry.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
  find_path(NXVW_INTER_VK_INCLUDE vulkan/vulkan.h
    HINTS ${NXVC_VK_HEADERS_DIR} ${NXVW_VULKAN_HEADERS}
          $ENV{VULKAN_SDK}/include
          ${CMAKE_SOURCE_DIR}/../tools/local/include
    PATHS /usr/include /usr/local/include)
  find_library(NXVW_INTER_VK_LIB NAMES vulkan vulkan-1
    HINTS $ENV{VULKAN_SDK}/lib PATHS /usr/lib /usr/lib64 /usr/local/lib)
endif()

if(Vulkan_FOUND OR (NXVW_INTER_VK_INCLUDE AND NXVW_INTER_VK_LIB))
  add_executable(nxvc-passW-test tools/nxvc-passW-test.cpp)
  add_dependencies(nxvc-passW-test nxvw_inter_spv)
  target_include_directories(nxvc-passW-test PRIVATE ${NXVW_INTER_GEN})
  target_link_libraries(nxvc-passW-test PRIVATE nxvw_inter_model)
  if(Vulkan_FOUND)
    target_link_libraries(nxvc-passW-test PRIVATE Vulkan::Vulkan)
  else()
    target_include_directories(nxvc-passW-test PRIVATE ${NXVW_INTER_VK_INCLUDE})
    target_link_libraries(nxvc-passW-test PRIVATE ${NXVW_INTER_VK_LIB})
  endif()
  set_target_properties(nxvc-passW-test PROPERTIES CXX_STANDARD 20
                                                   CXX_STANDARD_REQUIRED ON)
  if(NOT MSVC)
    target_compile_options(nxvc-passW-test PRIVATE -Wall -Wextra
                           -Wno-unused-parameter -Wno-missing-field-initializers)
  endif()
endif()
