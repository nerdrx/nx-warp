# vk/tools - headless CLIs over nxvc_vk_common.  No windows, no surfaces:
# everything here runs on a CI box with only a software ICD.

add_executable(nxvc-vkprobe vkprobe/main.cpp)
target_link_libraries(nxvc-vkprobe PRIVATE nxvc::vk_common)

add_executable(nxvc-vksubgroup vksubgroup/main.cpp)
target_link_libraries(nxvc-vksubgroup PRIVATE nxvc::vk_common)

foreach(t nxvc-vkprobe nxvc-vksubgroup)
  set_target_properties(${t} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  if(NOT MSVC)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endforeach()
