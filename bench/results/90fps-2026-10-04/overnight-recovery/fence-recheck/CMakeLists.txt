cmake_minimum_required(VERSION 3.20)
project(overnight_three_mode LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Vulkan REQUIRED)
find_library(ZSTD_LIB zstd REQUIRED)
add_executable(stereo-gpu src/main.cpp)
target_include_directories(stereo-gpu PRIVATE include)
target_link_libraries(stereo-gpu PRIVATE Vulkan::Vulkan ${ZSTD_LIB} lz4)
target_compile_options(stereo-gpu PRIVATE -O3 -Wall -Wextra -Wpedantic)

# Public reproduction: root rebuilt this shader byte-identical to the measured archive.
find_program(GLSLC glslc REQUIRED)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/encode_primary.spv"
  COMMAND "${GLSLC}" -O --target-env=vulkan1.1 -I "${CMAKE_SOURCE_DIR}/shaders" "${CMAKE_SOURCE_DIR}/shaders/encode_primary.comp" -o "${CMAKE_BINARY_DIR}/encode_primary.spv"
  DEPENDS shaders/encode_primary.comp shaders/astc_encode_inverse.glsl shaders/astc_encode_colour.glsl shaders/astc_encode_colour_tables.glsl VERBATIM)
add_custom_target(shader ALL DEPENDS "${CMAKE_BINARY_DIR}/encode_primary.spv")
add_dependencies(stereo-gpu shader)
