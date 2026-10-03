cmake_minimum_required(VERSION 3.20)
project(lz4_payload_bench LANGUAGES C CXX)

if(NOT DEFINED LZ4_DIR)
    message(FATAL_ERROR "Pass -DLZ4_DIR=/path/to/nx_lz4-src/lib")
endif()

add_executable(lz4_payload_bench main.cpp "${LZ4_DIR}/lz4.c")
target_include_directories(lz4_payload_bench PRIVATE "${LZ4_DIR}")
target_compile_features(lz4_payload_bench PRIVATE cxx_std_17)
target_compile_options(lz4_payload_bench PRIVATE -O3 -fno-strict-aliasing)
