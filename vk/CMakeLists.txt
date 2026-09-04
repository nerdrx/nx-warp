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
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_sub}/CMakeLists.txt)
    add_subdirectory(${_sub})
  endif()
endforeach()
