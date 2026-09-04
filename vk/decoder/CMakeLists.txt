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
  set(NXVCVK_DECODER_BUILT ON CACHE INTERNAL "")
else()
  message(WARNING "vk/decoder: Vulkan loader/headers not found; "
                  "nxvc_vk_decoder is not built")
  set(NXVCVK_DECODER_BUILT OFF CACHE INTERNAL "")
endif()

# ------------------------------------------------------- Android / arm64
# The library is plain C++20 over the core Vulkan 1.2 API with no desktop-only
# call in it, so an arm64 NDK build is a normal cross build; android/ links
# the target above.  This extra target exists only so a host build can prove
# the sources still *compile* for arm64 without a full NDK toolchain file, and
# is off unless asked for.
option(NXVCVK_DECODER_ANDROID_COMPILE_CHECK
       "Compile nxvc_vk_decoder's sources for arm64 with the NDK clang" OFF)
if(NXVCVK_DECODER_ANDROID_COMPILE_CHECK AND NOT ANDROID)
  find_program(NXVCVK_NDK_CLANG aarch64-linux-android29-clang++
    HINTS $ENV{ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin
          ${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin)
  if(NXVCVK_NDK_CLANG)
    add_custom_target(nxvc_vk_decoder_android_compile ALL
      COMMAND ${NXVCVK_NDK_CLANG} -std=c++20 -fsyntax-only -O2
              -I${CMAKE_SOURCE_DIR}/include
              -I${CMAKE_CURRENT_SOURCE_DIR}
              -I${CMAKE_SOURCE_DIR}
              -I${CMAKE_CURRENT_BINARY_DIR}/passA/generated
              -I${CMAKE_CURRENT_BINARY_DIR}/passB/generated
              ${NXVCVK_DEC_SRC}
      DEPENDS nxvw_passA_spv nxvw_passB_spv
      COMMENT "arm64 syntax check of nxvc_vk_decoder"
      VERBATIM)
  else()
    message(STATUS "vk/decoder: NDK clang++ not found, arm64 check skipped")
  endif()
endif()
