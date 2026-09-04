# stereo/ - STEREO mode design experiment (docs/STEREO.md).
#
# Added by the root CMakeLists when NXWARP_BUILD_STEREO is ON; also configurable
# standalone with `cmake -S stereo -B build-stereo` for a quick sim-only build.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(nxvc_stereo VERSION 0.0.1 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
    set(NXWARP_STEREO_STANDALONE ON)
endif()

add_library(nxs_sim STATIC
    sim/scene.cpp
    sim/predict.cpp
    sim/analyze.cpp
)
target_include_directories(nxs_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sim)
set_target_properties(nxs_sim PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(nxs_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(nxvc-stereosim sim/main.cpp)
target_link_libraries(nxvc-stereosim PRIVATE nxs_sim)
set_target_properties(nxvc-stereosim PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Standalone builds pull the tests in themselves; in the full tree the root
# tests/CMakeLists.txt adds tests/stereo directly.
if(NXWARP_STEREO_STANDALONE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../tests/stereo/CMakeLists.txt)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests/stereo ${CMAKE_BINARY_DIR}/tests-stereo)
endif()
