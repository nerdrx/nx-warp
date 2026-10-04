cmake_minimum_required(VERSION 3.16)
project(gpu_wait_diagnostic LANGUAGES CXX)

find_package(Vulkan REQUIRED)

add_executable(gpu_wait_probe probe.cpp)
target_compile_features(gpu_wait_probe PRIVATE cxx_std_20)
target_link_libraries(gpu_wait_probe PRIVATE Vulkan::Vulkan)

# Keep the exact shader used by the recorded run available to the probe.
configure_file(short.spv short.spv COPYONLY)
