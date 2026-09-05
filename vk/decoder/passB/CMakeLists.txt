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

# The tree's one shader-build rule and the one Adreno-safe pass list, rather
# than a local copy of either.  vk/decoder was the last component still
# carrying its own gen_spv.cmake; the pass list in it was identical, and this
# is what keeps it that way.
set(NXVW_PASSB_GEN_SPV
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/nxvc_gen_spv.cmake)
set(NXVW_PASSB_SPV_PASSES_CMAKE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/NxvcShaderPasses.cmake)

set(NXVW_PASSB_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(NXVW_PASSB_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${NXVW_PASSB_GEN})

# Four modules from one source, over the two BUILD variants: NXVW_INTRA_DIR
# selects whether the directional-intra wavefront exists, and NXVW_XFORM_LARGE
# whether the 16x16 and 32x32 transform forms do.  Both are preprocessor
# variants rather than specialization constants, and reconstruct.comp says why
# for each -- in both cases the driver's own dead-code pass was measured and
# was not enough, on an Adreno 650 in one case and on all three ICDs in the
# other.  See reconstruct.comp and ../README.md.
# Extra preprocessor definitions for every Pass B module, for ablations that
# have to be build variants because what they change is a loop bound -- which
# reconstruct.comp explains at length is the thing a specialization constant
# must not be on this driver.  Empty in every normal build.
set(NXVW_PASSB_EXTRA_DEFS "" CACHE STRING
    "extra -D flags for the Pass B shader modules (ablations)")

set(NXVW_PASSB_SPV_LIST)
foreach(_dir 1 0)
foreach(_xl 1 0)
  # reconstruct, reconstruct_v1, reconstruct_x8, reconstruct_v1_x8 -- the
  # suffix names what the module does NOT have, so the full kernel keeps the
  # bare name it has always had.
  set(_name reconstruct)
  if(NOT _dir)
    string(APPEND _name "_v1")
  endif()
  if(NOT _xl)
    string(APPEND _name "_x8")
  endif()
  set(_out ${NXVW_PASSB_GEN}/${_name}.spv.h)
  list(APPEND NXVW_PASSB_SPV_LIST ${_out})
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${CMAKE_COMMAND}
            -DGLSLC=${NXVW_GLSLC}
            -DSRC=${NXVW_PASSB_DIR}/reconstruct.comp
            -DOUT=${_out}
            -DNAME=${_name}
            "-DDEFS=-DNXVW_INTRA_DIR=${_dir} -DNXVW_XFORM_LARGE=${_xl} ${NXVW_PASSB_EXTRA_DEFS}"
            -DINCDIR=${NXVW_PASSB_DIR}
            -DSTYLE=plain
            -P ${NXVW_PASSB_GEN_SPV}
    DEPENDS ${NXVW_PASSB_DIR}/reconstruct.comp
            ${NXVW_PASSB_DIR}/syntax_constants.h
            ${NXVW_PASSB_DIR}/passB_layout.h
            ${NXVW_PASSB_DIR}/store.glsl
            ${NXVW_PASSB_DIR}/../inter/inter_hook.glsl
            ${NXVW_PASSB_DIR}/../inter/inter_layout.h
            ${NXVW_PASSB_GEN_SPV} ${NXVW_PASSB_SPV_PASSES_CMAKE}
    COMMENT "glslc reconstruct.comp (dir=${_dir} xl=${_xl}) -> ${_name}.spv.h"
    VERBATIM)
endforeach()
endforeach()
# [inter] A fifth module: the WARP_SKIP kernel.
#
# A WARP_SKIP tile codes nothing -- no header, no payload, no coefficients --
# so its reconstruction is exactly clamp(W), the predictor Pass W already
# wrote.  It was nevertheless running the whole intra machinery to get there:
# the DC-plane inverse transform, the block-mean field, and the bilinear
# planar interpolation over that field, all of which for a skipped tile
# evaluate to the constant dc_offset and then cancel against the `- dcOff` in
# the prediction hook.  Measured on a 7900 XTX over the head-turn fixture, a
# skip tile cost 0.344 us against an intra tile's 0.128 in the same frame:
# **2.7x for the case that has nothing to do.**
#
# It needs no dir/xform_large variants.  A skip tile is never INTRA, so it can
# never enter the directional wavefront, and it runs no transform, so the
# transform size cannot reach it.  One module, not four.
set(_skip_out ${NXVW_PASSB_GEN}/reconstruct_skip.spv.h)
list(APPEND NXVW_PASSB_SPV_LIST ${_skip_out})
add_custom_command(
  OUTPUT ${_skip_out}
  COMMAND ${CMAKE_COMMAND}
          -DGLSLC=${NXVW_GLSLC}
          -DSRC=${NXVW_PASSB_DIR}/reconstruct.comp
          -DOUT=${_skip_out}
          -DNAME=reconstruct_skip
          "-DDEFS=-DNXVW_INTRA_DIR=0 -DNXVW_XFORM_LARGE=0 -DNXVW_SKIP_ONLY=1 ${NXVW_PASSB_EXTRA_DEFS}"
          -DINCDIR=${NXVW_PASSB_DIR}
          -DSTYLE=plain
          -P ${NXVW_PASSB_GEN_SPV}
  DEPENDS ${NXVW_PASSB_DIR}/reconstruct.comp
          ${NXVW_PASSB_DIR}/syntax_constants.h
          ${NXVW_PASSB_DIR}/passB_layout.h
            ${NXVW_PASSB_DIR}/store.glsl
          ${NXVW_PASSB_DIR}/../inter/inter_hook.glsl
          ${NXVW_PASSB_DIR}/../inter/inter_layout.h
          ${NXVW_PASSB_GEN_SPV} ${NXVW_PASSB_SPV_PASSES_CMAKE}
  COMMENT "glslc reconstruct.comp (WARP_SKIP only) -> reconstruct_skip.spv.h"
  VERBATIM)

set(NXVW_PASSB_SPV ${NXVW_PASSB_GEN}/reconstruct.spv.h)
set(NXVW_PASSB_SPV_V1 ${NXVW_PASSB_GEN}/reconstruct_v1.spv.h)
add_custom_target(nxvw_passB_spv DEPENDS ${NXVW_PASSB_SPV_LIST})

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
