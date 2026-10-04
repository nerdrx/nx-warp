cmake_minimum_required(VERSION 3.20)
project(sender_overlap_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_library(ZSTD_LIB zstd REQUIRED)
find_library(LZ4_LIB lz4 REQUIRED)
find_program(GLSLC glslc REQUIRED)

add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/encode_primary.spv
  COMMAND ${GLSLC} -O -I ${CMAKE_SOURCE_DIR}/shaders
          ${CMAKE_SOURCE_DIR}/shaders/encode_primary.comp
          -o ${CMAKE_BINARY_DIR}/encode_primary.spv
  DEPENDS shaders/encode_primary.comp shaders/astc_encode_inverse.glsl
          shaders/astc_encode_colour.glsl shaders/astc_encode_colour_tables.glsl
  VERBATIM)
add_custom_target(encode_shader DEPENDS ${CMAKE_BINARY_DIR}/encode_primary.spv)

add_executable(stereo-sender-wait src/sender_wait.cpp)
add_dependencies(stereo-sender-wait encode_shader)
target_link_libraries(stereo-sender-wait PRIVATE Vulkan::Vulkan Threads::Threads
                      ${ZSTD_LIB} ${LZ4_LIB})
target_compile_options(stereo-sender-wait PRIVATE -O3 -Wall -Wextra -Wpedantic)
