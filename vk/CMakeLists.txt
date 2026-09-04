# vk/ - the Vulkan side of NX Warp.
#
#   common/    nxvc_vk_common: capability probe (docs/PAPER.md 3.7), device
#              creation or adoption, resources, sync, pipelines, external
#              memory.  Everything below links it.
#   tools/     headless CLIs: nxvc-vkprobe, nxvc-vksubgroup.
#   encoder/   E0..E5 (3.6).
#   decoder/   Pass A/B/C (3.2, 3.5).
#
# Every subdirectory is EXISTS-guarded so the tree configures cleanly while
# components land independently.  NXWARP_VK_SUBDIRS narrows the set when a
# sibling is mid-rewrite:
#
#   cmake -DNXWARP_VK_SUBDIRS="common;tools" ...

set(NXWARP_VK_SUBDIRS "common;tools;encoder;decoder"
    CACHE STRING "vk/ subdirectories to build")

foreach(_sub IN LISTS NXWARP_VK_SUBDIRS)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_sub}/CMakeLists.txt)
    continue()
  endif()
  # common/ drops itself when no Vulkan headers are installed (it warns and
  # returns rather than aborting the whole project configure).  Everything
  # else here needs it, so skip them too instead of erroring on a missing
  # link target.
  if(NOT _sub STREQUAL "common" AND NOT TARGET nxvc_vk_common)
    message(STATUS "vk/${_sub}: nxvc_vk_common not available, skipped")
    continue()
  endif()
  # Headers but no loader: the library still compiles (that is the point of
  # the Android and Windows cross-compile checks) but nothing can be linked.
  if(_sub STREQUAL "tools" AND NXVC_VK_NO_LOADER)
    message(STATUS "vk/tools: no Vulkan loader to link against, skipped")
    continue()
  endif()
  add_subdirectory(${_sub})
endforeach()
