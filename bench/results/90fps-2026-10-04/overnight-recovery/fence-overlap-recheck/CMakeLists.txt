cmake_minimum_required(VERSION 3.20)
project(overnight_three_mode LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Vulkan REQUIRED)
find_library(ZSTD_LIB zstd REQUIRED)
add_executable(stereo-gpu src/main.cpp)
target_include_directories(stereo-gpu PRIVATE ../fence-recheck/include)
target_link_libraries(stereo-gpu PRIVATE Vulkan::Vulkan ${ZSTD_LIB} lz4)
target_compile_options(stereo-gpu PRIVATE -O3 -Wall -Wextra -Wpedantic)

find_program(GLSLC glslc REQUIRED)
set(SHADERS "${CMAKE_SOURCE_DIR}/../fence-recheck/shaders")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/encode_primary.spv"
 COMMAND "${GLSLC}" -O --target-env=vulkan1.1 -I "${SHADERS}" "${SHADERS}/encode_primary.comp" -o "${CMAKE_BINARY_DIR}/encode_primary.spv"
 DEPENDS "${SHADERS}/encode_primary.comp" "${SHADERS}/astc_encode_inverse.glsl" "${SHADERS}/astc_encode_colour.glsl" "${SHADERS}/astc_encode_colour_tables.glsl" VERBATIM)
add_custom_target(shader ALL DEPENDS "${CMAKE_BINARY_DIR}/encode_primary.spv")
add_dependencies(stereo-gpu shader)
