# nxvc-d3dinterop: the NX Warp Windows interop probe (paper 3.7 and 3.8).

option(NXWARP_WIN_KEYED_MUTEX
       "Use VK_KHR_win32_keyed_mutex instead of a D3D11.4 shared fence (paper 3.8 fallback)"
       OFF)

# --- Vulkan headers ----------------------------------------------------------
# Headers only: vulkan-1.dll is resolved at runtime with LoadLibrary, so no
# import library is needed and the probe still starts on a box with no ICD.
set(NXWARP_VULKAN_INCLUDE_DIR "" CACHE PATH "Directory containing vulkan/vulkan.h")

if(NOT NXWARP_VULKAN_INCLUDE_DIR)
  find_path(NXWARP_VULKAN_INCLUDE_DIR_FOUND
    NAMES vulkan/vulkan.h
    HINTS
      $ENV{VULKAN_SDK}/include
      /run/media/nerdrx/Lex/claude/tools/vulkan-sdk/include
      /run/media/nerdrx/Lex/claude/tools/local/include
      /usr/include
    NO_CMAKE_FIND_ROOT_PATH
  )
  if(NXWARP_VULKAN_INCLUDE_DIR_FOUND)
    set(NXWARP_VULKAN_INCLUDE_DIR "${NXWARP_VULKAN_INCLUDE_DIR_FOUND}")
  endif()
endif()

if(NOT NXWARP_VULKAN_INCLUDE_DIR OR NOT EXISTS "${NXWARP_VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
  message(FATAL_ERROR
    "Vulkan headers not found. Pass -DNXWARP_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>.")
endif()
message(STATUS "nxvc-d3dinterop: Vulkan headers at ${NXWARP_VULKAN_INCLUDE_DIR}")

# --- shader ------------------------------------------------------------------
find_program(NXWARP_GLSLC NAMES glslc glslc.exe NO_CMAKE_FIND_ROOT_PATH)
find_program(NXWARP_GLSLANG NAMES glslangValidator glslangValidator.exe NO_CMAKE_FIND_ROOT_PATH)

set(shader_src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/checker.comp)
set(shader_spv ${CMAKE_CURRENT_BINARY_DIR}/checker.spv)
set(shader_hdr ${CMAKE_CURRENT_BINARY_DIR}/generated/checker_spv.h)

if(NXWARP_GLSLC)
  set(compile_shader_cmd ${NXWARP_GLSLC} --target-env=vulkan1.2 -O -fshader-stage=compute
                         ${shader_src} -o ${shader_spv})
elseif(NXWARP_GLSLANG)
  set(compile_shader_cmd ${NXWARP_GLSLANG} --target-env vulkan1.2 -S comp
                         ${shader_src} -o ${shader_spv})
else()
  message(FATAL_ERROR "neither glslc nor glslangValidator found; cannot build the probe shader")
endif()

add_custom_command(
  OUTPUT ${shader_spv}
  COMMAND ${compile_shader_cmd}
  DEPENDS ${shader_src}
  COMMENT "Compiling checker.comp to SPIR-V"
  VERBATIM)

add_custom_command(
  OUTPUT ${shader_hdr}
  COMMAND ${CMAKE_COMMAND} -DIN=${shader_spv} -DOUT=${shader_hdr} -DSYMBOL=kCheckerSpv
          -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/embed_spv.cmake
  DEPENDS ${shader_spv} ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/embed_spv.cmake
  COMMENT "Embedding checker.spv"
  VERBATIM)

add_custom_target(nxvc_d3dinterop_shader DEPENDS ${shader_hdr})

# --- build id ----------------------------------------------------------------
string(TIMESTAMP NXWARP_BUILD_DATE "%Y-%m-%d" UTC)
set(NXWARP_BUILD_ID "0.1.0+${NXWARP_BUILD_DATE}")

# --- executable --------------------------------------------------------------
add_executable(nxvc-d3dinterop
  src/main.cpp
  src/profile.cpp
)
add_dependencies(nxvc-d3dinterop nxvc_d3dinterop_shader)

target_sources(nxvc-d3dinterop PRIVATE ${shader_hdr})

target_include_directories(nxvc-d3dinterop PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${NXWARP_VULKAN_INCLUDE_DIR}
)

target_compile_definitions(nxvc-d3dinterop PRIVATE
  NXWARP_PROBE_BUILD_ID="${NXWARP_BUILD_ID}"
  NXWARP_WIN_KEYED_MUTEX=$<BOOL:${NXWARP_WIN_KEYED_MUTEX}>
  UNICODE
  _UNICODE
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
)

target_compile_features(nxvc-d3dinterop PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(nxvc-d3dinterop PRIVATE /W4 /permissive-)
else()
  target_compile_options(nxvc-d3dinterop PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  # Static runtime: the exe is scp'd to a box with no toolchain on it.
  target_link_options(nxvc-d3dinterop PRIVATE
    -static -static-libgcc -static-libstdc++ -Wl,--subsystem,console)
endif()

target_link_libraries(nxvc-d3dinterop PRIVATE d3d11 dxgi dxguid ole32)

install(TARGETS nxvc-d3dinterop RUNTIME DESTINATION bin)
