# [ATLAS] 13.12.2's composition against an independent 128-bit oracle.  Links
# no Vulkan and needs no device, so it runs everywhere the project builds.
if(NOT TARGET nxvw_atlas_model)
  return()
endif()
add_executable(nxvc-atlas-compose-test test_atlas_compose.cpp)
target_link_libraries(nxvc-atlas-compose-test PRIVATE nxvw_atlas_model)
set_target_properties(nxvc-atlas-compose-test PROPERTIES CXX_STANDARD 20
                                                         CXX_STANDARD_REQUIRED ON)
add_test(NAME vk.atlas.compose COMMAND nxvc-atlas-compose-test)
set_tests_properties(vk.atlas.compose PROPERTIES TIMEOUT 300)
