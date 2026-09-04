# vk/common - nxvc_vk_common, the shared Vulkan runtime.
#
# Static library with a C ABI, linked by vk/encoder, vk/decoder, the tools and
# eventually the WiVRn NX server and client.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NxvcEmbedShaders)

add_library(nxvc_vk_common STATIC
  src/c_api.cpp
  src/commands.cpp
  src/context.cpp
  src/external.cpp
  src/external_ahb.cpp
  src/external_win32.cpp
  src/pipeline.cpp
  src/probe.cpp
  src/probe_json.cpp
  src/resources.cpp
  src/util.cpp
)
add_library(nxvc::vk_common ALIAS nxvc_vk_common)

target_include_directories(nxvc_vk_common
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(nxvc_vk_common PUBLIC cxx_std_20)
set_target_properties(nxvc_vk_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Vulkan headers and loader.  Three ways in, in order of preference:
#   1. find_package(Vulkan) -- system SDK or distro package
#   2. NXVC_VK_HEADERS_DIR pointing at a Vulkan-Headers checkout
#   3. headers only, no loader: enough to *compile* the library, which is all
#      the Android and Windows cross-compile checks need.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
  target_include_directories(nxvc_vk_common PUBLIC ${Vulkan_INCLUDE_DIRS})
  if(TARGET Vulkan::Vulkan AND NOT NXVC_VK_HEADERS_ONLY)
    target_link_libraries(nxvc_vk_common PUBLIC Vulkan::Vulkan)
  endif()
  message(STATUS "nxvc_vk_common: Vulkan headers ${Vulkan_INCLUDE_DIRS}")
elseif(NXVC_VK_HEADERS_DIR)
  target_include_directories(nxvc_vk_common PUBLIC ${NXVC_VK_HEADERS_DIR})
  # Headers came from a checkout; the loader may still be installed normally.
  # Without one the library still compiles, which is exactly what the Android
  # and Windows cross-compile checks need, but nothing can link an executable.
  find_library(NXVC_VULKAN_LIBRARY NAMES vulkan vulkan-1)
  if(NXVC_VULKAN_LIBRARY AND NOT NXVC_VK_HEADERS_ONLY)
    target_link_libraries(nxvc_vk_common PUBLIC ${NXVC_VULKAN_LIBRARY})
    message(STATUS "nxvc_vk_common: headers ${NXVC_VK_HEADERS_DIR}, "
                   "loader ${NXVC_VULKAN_LIBRARY}")
  else()
    set(NXVC_VK_NO_LOADER ON PARENT_SCOPE)
    message(STATUS "nxvc_vk_common: headers ${NXVC_VK_HEADERS_DIR}, no loader "
                   "(compile-only build; tools and tests are skipped)")
  endif()
else()
  message(FATAL_ERROR
    "Vulkan headers not found.  Install the Vulkan SDK or pass "
    "-DNXVC_VK_HEADERS_DIR=/path/to/Vulkan-Headers/include")
endif()

# Platform switches.  VK_USE_PLATFORM_* gates the external-memory structs the
# interop translation units need; without them vulkan_core.h alone does not
# declare VkImportMemoryWin32HandleInfoKHR or the AHardwareBuffer entry points.
if(ANDROID)
  target_compile_definitions(nxvc_vk_common PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
  target_link_libraries(nxvc_vk_common PUBLIC android)
  if(NOT NXVC_VK_HEADERS_ONLY)
    target_link_libraries(nxvc_vk_common PUBLIC vulkan)
  endif()
elseif(WIN32)
  target_compile_definitions(nxvc_vk_common PUBLIC
    VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
  target_compile_options(nxvc_vk_common PRIVATE /W4 /permissive-)
else()
  target_compile_options(nxvc_vk_common PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# The subgroup-semantics conformance kernel of 3.2.6, compiled twice: plain,
# and with the real subgroupClustered* ops as an oracle for devices that
# advertise VK_SUBGROUP_FEATURE_CLUSTERED_BIT.
if(NOT NXVC_VK_HEADERS_ONLY)
  nxvc_add_shaders(nxvc_vk_common
    SHADERS shaders/nxvc_subgroup_semantics.comp)
  nxvc_add_shaders(nxvc_vk_common
    SUFFIX _clustered
    DEFINES NXVC_HAVE_CLUSTERED=1
    SHADERS shaders/nxvc_subgroup_semantics.comp)
endif()
