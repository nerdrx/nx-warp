cmake_minimum_required(VERSION 3.20)
project(native_pass_fusion LANGUAGES CXX)
find_package(Vulkan REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(SHADERS astc_encode.comp fused_astc.comp passthrough.comp)
foreach(shader IN LISTS SHADERS)
  get_filename_component(name "${shader}" NAME_WE)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -I"${CMAKE_CURRENT_SOURCE_DIR}/shaders" -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" ${CMAKE_CURRENT_SOURCE_DIR}/shaders/astc_encode_inverse.glsl ${CMAKE_CURRENT_SOURCE_DIR}/shaders/astc_encode_colour.glsl ${CMAKE_CURRENT_SOURCE_DIR}/shaders/astc_encode_colour_tables.glsl
    VERBATIM)
  list(APPEND SPV "${CMAKE_CURRENT_BINARY_DIR}/${name}.spv")
endforeach()
add_custom_target(shaders ALL DEPENDS ${SPV})
add_executable(native_pass_fusion main.cpp)
add_dependencies(native_pass_fusion shaders)
target_compile_features(native_pass_fusion PRIVATE cxx_std_20)
target_link_libraries(native_pass_fusion PRIVATE Vulkan::Vulkan)
