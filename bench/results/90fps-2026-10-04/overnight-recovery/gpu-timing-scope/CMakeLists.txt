cmake_minimum_required(VERSION 3.20)
project(gpu_timestamp_scope LANGUAGES CXX)

set(WIVRN_SOURCE_ROOT "" CACHE PATH "WiVRn checkout used for the production helper")
if(NOT EXISTS "${WIVRN_SOURCE_ROOT}/server/encoder/astc_gpu_timing.h")
	message(FATAL_ERROR "Set WIVRN_SOURCE_ROOT to the source checkout")
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

set(SHADER_SPV "${CMAKE_CURRENT_BINARY_DIR}/scope.comp.spv")
add_custom_command(
	OUTPUT "${SHADER_SPV}"
	COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -o "${SHADER_SPV}" "${CMAKE_CURRENT_SOURCE_DIR}/scope.comp"
	DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scope.comp"
	VERBATIM)
add_custom_target(scope_shader DEPENDS "${SHADER_SPV}")

add_executable(gpu_timestamp_scope main.cpp)
add_dependencies(gpu_timestamp_scope scope_shader)
target_compile_features(gpu_timestamp_scope PRIVATE cxx_std_23)
target_include_directories(gpu_timestamp_scope PRIVATE "${WIVRN_SOURCE_ROOT}/server/encoder")
target_link_libraries(gpu_timestamp_scope PRIVATE Vulkan::Vulkan Threads::Threads)
