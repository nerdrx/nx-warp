# NX Warp Vulkan compute decoder.
#
#   passA/        rANS entropy decode kernel, CPU model, harness
#   passB/        reconstruction kernel, CPU model, harness
#   this file     nxvc_vk_decoder: the library that drives the two, its C ABI
#                 (<nxvc/nxvc_vk.h>) and the nxvc-vkdec CLI
#
# The two passes stay independently buildable and testable; the library adds
# the host-side container parse and one command buffer around them.

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/passA/CMakeLists.txt)
  add_subdirectory(passA)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/passB/CMakeLists.txt)
  add_subdirectory(passB)
endif()

# Pass A's workgroup shape reaches this library too: nxs_tiles_per_group() in
# passA/syntax_constants.h is what the host uses to size a dispatch, and it has
# to be the number the shader was built with.
if(DEFINED NXVW_PASSA_TPG)
  add_compile_definitions(NXVW_PASSA_TILES_PER_GROUP=${NXVW_PASSA_TPG})
endif()

if(NOT TARGET nxvw_passA_model OR NOT TARGET nxvw_passB_model)
  message(STATUS "vk/decoder: passA/passB not present, skipping the library")
  return()
endif()

# --------------------------------------------------------------- Vulkan
# Same discovery order as the pass harnesses and vk/common: find_package
# first, then the project-wide -DNXVC_VK_HEADERS_DIR pointing at a
# Vulkan-Headers checkout.  Android gets its loader from the NDK toolchain.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
  find_path(NXVCVK_DEC_INCLUDE vulkan/vulkan.h
    HINTS ${NXVC_VK_HEADERS_DIR} ${NXVW_VULKAN_HEADERS}
          $ENV{VULKAN_SDK}/include
          ${CMAKE_SOURCE_DIR}/../tools/local/include
    PATHS /usr/include /usr/local/include)
  find_library(NXVCVK_DEC_LIB NAMES vulkan vulkan-1
    HINTS $ENV{VULKAN_SDK}/lib PATHS /usr/lib /usr/lib64 /usr/local/lib)
endif()

set(NXVCVK_DEC_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/nxvc_vkdec_parse.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/nxvc_vkdec.cpp)

# The SPIR-V of both kernels is embedded as a C array by each pass' own
# gen_spv.cmake; the library only has to see the generated headers and depend
# on the targets that produce them.
set(NXVCVK_DEC_GEN
  ${CMAKE_CURRENT_BINARY_DIR}/passA/generated
  ${CMAKE_CURRENT_BINARY_DIR}/passB/generated)

function(nxvcvk_dec_configure tgt)
  target_include_directories(${tgt} PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(${tgt} PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${NXVCVK_DEC_GEN})
  target_include_directories(${tgt} PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/passA
    ${CMAKE_CURRENT_LIST_DIR}/passB)
  set_target_properties(${tgt} PROPERTIES CXX_STANDARD 20
                                          CXX_STANDARD_REQUIRED ON
                                          POSITION_INDEPENDENT_CODE ON)
  if(NOT MSVC)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-unused-parameter
                           -Wno-missing-field-initializers)
  endif()
endfunction()

if(Vulkan_FOUND OR (NXVCVK_DEC_INCLUDE AND NXVCVK_DEC_LIB))
  add_library(nxvc_vk_decoder STATIC ${NXVCVK_DEC_SRC})
  add_dependencies(nxvc_vk_decoder nxvw_passA_spv nxvw_passB_spv)
  nxvcvk_dec_configure(nxvc_vk_decoder)
  # Only headers are taken from passA/ and passB/ -- the layout structs, the
  # syntax constants and the inline geometry helpers are all `inline` or
  # `constexpr`, so the library links neither CPU model.  That keeps
  # nxvc_vk_decoder exportable on its own (the test-only model libraries are
  # not part of the installed package) and keeps the two passes free to move.
  if(Vulkan_FOUND)
    target_link_libraries(nxvc_vk_decoder PUBLIC Vulkan::Vulkan)
  else()
    target_include_directories(nxvc_vk_decoder PUBLIC ${NXVCVK_DEC_INCLUDE})
    target_link_libraries(nxvc_vk_decoder PUBLIC ${NXVCVK_DEC_LIB})
  endif()
  add_library(nxvc::vk_decoder ALIAS nxvc_vk_decoder)

  # ------------------------------------------------------------------ CLI
  # Mirrors ref/tools/nxv-dec so tools/quality can drive the GPU decoder over
  # the same --codec-cmd contract.
  add_executable(nxvc-vkdec ${CMAKE_CURRENT_SOURCE_DIR}/tools/nxvc-vkdec.cpp)
  target_link_libraries(nxvc-vkdec PRIVATE nxvc_vk_decoder)
  nxvcvk_dec_configure(nxvc-vkdec)

  # tools/quality's `--codec-cmd <prefix>` appends -enc / -dec, so give it a
  # prefix whose -dec is the GPU decoder and whose -enc is the reference
  # encoder: `--codec-cmd <build>/bin/nxvgpu` then measures the GPU decode
  # path with no extra flags.  --codec-enc / --codec-dec still work directly.
  if(TARGET nxv-enc AND UNIX)
    add_custom_command(TARGET nxvc-vkdec POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink
              $<TARGET_FILE_NAME:nxvc-vkdec>
              $<TARGET_FILE_DIR:nxvc-vkdec>/nxvgpu-dec
      COMMAND ${CMAKE_COMMAND} -E create_symlink
              $<TARGET_FILE_NAME:nxv-enc>
              $<TARGET_FILE_DIR:nxvc-vkdec>/nxvgpu-enc
      COMMENT "nxvgpu-{enc,dec} aliases for tools/quality --codec-cmd"
      VERBATIM)
  endif()
  set(NXVCVK_DECODER_BUILT ON CACHE INTERNAL "")
else()
  message(WARNING "vk/decoder: Vulkan loader/headers not found; "
                  "nxvc_vk_decoder is not built")
  set(NXVCVK_DECODER_BUILT OFF CACHE INTERNAL "")
endif()

# ------------------------------------------------------- Android / arm64
# Nothing special is needed: the library is plain C++20 over the core Vulkan
# 1.2 API with no desktop-only call in it, and the NDK supplies both glslc
# (shader-tools/) and libvulkan, so an arm64 build is a normal cross build of
# this same target.  android/ links it.
#
#   NDK=$ANDROID_SDK/ndk/<version>
#   cmake -S . -B build-android -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
#     -DNXWARP_BUILD_VK=ON -DNXWARP_BUILD_TESTS=OFF \
#     -DNXWARP_BUILD_EXAMPLES=OFF -DNXWARP_BUILD_TRANSPORT=OFF \
#     -DNXWARP_BUILD_TOOLS=OFF
#   cmake --build build-android --target nxvc_vk_decoder
#
# The one Android-specific decision the library makes is the default output
# format: NXVC_VKD_OUT_AUTO resolves a 4:2:0 passthrough stream to the
# two-plane YCbCr store, which is what the client's reprojection shader
# already samples and which halves reference-slot memory on the headset
# (docs/INTEGRATION-DECISIONS.md 3).
if(ANDROID)
  message(STATUS "vk/decoder: building nxvc_vk_decoder for ${ANDROID_ABI}")
endif()
