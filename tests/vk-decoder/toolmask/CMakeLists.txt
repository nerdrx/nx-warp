# vk.decoder.tool_mask -- the per-device capability mask, pinned.
#
# Links the parse unit only: no Vulkan, no device, no shaders.  The mask is a
# pure function of the vendor id and the device name, so the test that guards
# it should be too.
if(NOT TARGET nxvc_vk_decoder)
  return()
endif()

add_executable(test_vk_decoder_tool_mask test_tool_mask.cpp)
target_link_libraries(test_vk_decoder_tool_mask PRIVATE nxvc_vk_decoder)
target_include_directories(test_vk_decoder_tool_mask PRIVATE
  ${CMAKE_SOURCE_DIR}/vk/decoder)
set_target_properties(test_vk_decoder_tool_mask PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(test_vk_decoder_tool_mask PRIVATE -Wall -Wextra)
endif()
add_test(NAME vk.decoder.tool_mask COMMAND test_vk_decoder_tool_mask)
