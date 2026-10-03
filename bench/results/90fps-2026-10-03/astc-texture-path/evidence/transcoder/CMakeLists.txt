cmake_minimum_required(VERSION 3.20)
project(xuastc_transcode_bench LANGUAGES C CXX)

if(NOT DEFINED BASIS_DIR)
    message(FATAL_ERROR "Pass -DBASIS_DIR=/path/to/basis_universal")
endif()

add_executable(xuastc_bench main.cpp
    "${BASIS_DIR}/transcoder/basisu_transcoder.cpp"
    "${BASIS_DIR}/zstd/zstddeclib.c")
target_include_directories(xuastc_bench PRIVATE "${BASIS_DIR}")
target_compile_features(xuastc_bench PRIVATE cxx_std_17)
target_compile_definitions(xuastc_bench PRIVATE BASISD_SUPPORT_KTX2_ZSTD=1 BASISD_SUPPORT_XUASTC=1)
target_compile_options(xuastc_bench PRIVATE -O3 -fno-strict-aliasing)
target_link_libraries(xuastc_bench PRIVATE ${CMAKE_DL_LIBS})
