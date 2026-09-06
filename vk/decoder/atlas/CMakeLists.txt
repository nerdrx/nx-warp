# [ATLAS] The composition model of [SYN] 13.12.2.  No Vulkan and no shaders:
# it is plain integer arithmetic that the GPU kernel is later checked against,
# the way passA_model.cpp is checked against rans_decode.comp.
set(NXVW_ATLAS_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
add_library(nxvw_atlas_model STATIC atlas_model.cpp)
target_include_directories(nxvw_atlas_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                            ${CMAKE_CURRENT_SOURCE_DIR}/../inter)
set_target_properties(nxvw_atlas_model PROPERTIES CXX_STANDARD 20
                                                  CXX_STANDARD_REQUIRED ON)
