# [ATLAS] The composition of [SYN] 13.12.2, twice: once in ordinary C++ as the
# oracle, once in GLSL as the kernel that must match it bit for bit.
#
#   atlas_layout.h      the one description of the 64-byte table, the H ring
#                       and the push block, shared by model, kernel and host
#   atlas_arith.glsl    13.12.2 in emulated 64-bit GLSL; arithmetic only
#   atlas_compose.comp  13.12.3 step 1, one thread per table entry
#   atlas_model.{h,cpp} the CPU model, pinned to ref/src/inter.h
set(NXVW_ATLAS_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)

# ---------------------------------------------------------------- CPU model
add_library(nxvw_atlas_model STATIC atlas_model.cpp)
target_include_directories(nxvw_atlas_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                            ${CMAKE_CURRENT_SOURCE_DIR}/../inter)
set_target_properties(nxvw_atlas_model PROPERTIES CXX_STANDARD 20
                                                  CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------- kernel
# The tree's one shader-build rule and its one Adreno-safe pass list.  They are
# named again here rather than inherited from a sibling: a `set()` there is
# directory-scoped and this directory does not see it.
set(NXVW_ATLAS_GEN_SPV
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/nxvc_gen_spv.cmake)
set(NXVW_ATLAS_SPV_PASSES_CMAKE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/NxvcShaderPasses.cmake)

if(NXVW_GLSLC)
  set(NXVW_ATLAS_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
  file(MAKE_DIRECTORY ${NXVW_ATLAS_GEN})
  set(NXVW_ATLAS_SPV ${NXVW_ATLAS_GEN}/atlas_compose.spv.h)
  add_custom_command(
    OUTPUT ${NXVW_ATLAS_SPV}
    COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${NXVW_GLSLC}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/atlas_compose.comp
            -DOUT=${NXVW_ATLAS_SPV}
            -DNAME=atlas_compose
            -DINCDIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DSTYLE=plain
            -DDEPFILE=${NXVW_ATLAS_SPV}.d
            -P ${NXVW_ATLAS_GEN_SPV}
    # The include tree comes from glslc, not from a list kept by hand.
    DEPFILE ${NXVW_ATLAS_SPV}.d
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/atlas_compose.comp
            ${NXVW_ATLAS_GEN_SPV} ${NXVW_ATLAS_SPV_PASSES_CMAKE}
    COMMENT "glslc atlas_compose.comp -> atlas_compose.spv.h"
    VERBATIM)
  # The coded-tile kernel: MATGEN before Pass W, WRITEBACK after Pass B.
  set(NXVW_ATLAS_TSPV ${NXVW_ATLAS_GEN}/atlas_tiles.spv.h)
  add_custom_command(
    OUTPUT ${NXVW_ATLAS_TSPV}
    COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${NXVW_GLSLC}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/atlas_tiles.comp
            -DOUT=${NXVW_ATLAS_TSPV}
            -DNAME=atlas_tiles
            -DINCDIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DSTYLE=plain
            -DDEPFILE=${NXVW_ATLAS_TSPV}.d
            -P ${NXVW_ATLAS_GEN_SPV}
    DEPFILE ${NXVW_ATLAS_TSPV}.d
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/atlas_tiles.comp
            ${NXVW_ATLAS_GEN_SPV} ${NXVW_ATLAS_SPV_PASSES_CMAKE}
    COMMENT "glslc atlas_tiles.comp -> atlas_tiles.spv.h"
    VERBATIM)
  add_custom_target(nxvw_atlas_spv DEPENDS ${NXVW_ATLAS_SPV} ${NXVW_ATLAS_TSPV})

  # ---------------------------------------------------------------- harness
  # GPU-vs-CPU for the compose kernel, the same shape passA/, passB/ and
  # inter/ already carry.
  find_package(Vulkan QUIET)
  if(NOT Vulkan_FOUND)
    find_path(NXVW_ATLAS_VK_INCLUDE vulkan/vulkan.h
      HINTS ${NXVC_VK_HEADERS_DIR} ${NXVW_VULKAN_HEADERS}
            $ENV{VULKAN_SDK}/include
            ${CMAKE_SOURCE_DIR}/../tools/local/include
      PATHS /usr/include /usr/local/include)
    find_library(NXVW_ATLAS_VK_LIB NAMES vulkan vulkan-1
      HINTS $ENV{VULKAN_SDK}/lib PATHS /usr/lib /usr/lib64 /usr/local/lib)
  endif()

  if(Vulkan_FOUND OR (NXVW_ATLAS_VK_INCLUDE AND NXVW_ATLAS_VK_LIB))
    add_executable(nxvc-atlas-gpu-test tools/nxvc-atlas-gpu-test.cpp)
    add_dependencies(nxvc-atlas-gpu-test nxvw_atlas_spv)
    target_include_directories(nxvc-atlas-gpu-test PRIVATE ${NXVW_ATLAS_GEN})
    target_link_libraries(nxvc-atlas-gpu-test PRIVATE nxvw_atlas_model)
    # inter_state.h, for nxvcvk::plane_homography(): the harness checks the
    # MATGEN kernel against the SAME host helper that builds the frame's four
    # matrices, so the per-tile and frame-uniform paths are provably one
    # arithmetic rather than two transcriptions of it.
    target_include_directories(nxvc-atlas-gpu-test PRIVATE
                               ${CMAKE_CURRENT_SOURCE_DIR}/../inter)
    if(Vulkan_FOUND)
      target_link_libraries(nxvc-atlas-gpu-test PRIVATE Vulkan::Vulkan)
    else()
      target_include_directories(nxvc-atlas-gpu-test PRIVATE
                                 ${NXVW_ATLAS_VK_INCLUDE})
      target_link_libraries(nxvc-atlas-gpu-test PRIVATE ${NXVW_ATLAS_VK_LIB})
    endif()
    set_target_properties(nxvc-atlas-gpu-test PROPERTIES CXX_STANDARD 20
                                                         CXX_STANDARD_REQUIRED ON)
    if(NOT MSVC)
      target_compile_options(nxvc-atlas-gpu-test PRIVATE -Wall -Wextra
                             -Wno-unused-parameter
                             -Wno-missing-field-initializers)
    endif()
  endif()
endif()
