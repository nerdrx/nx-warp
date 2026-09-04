add_library(nxvc_ref STATIC
    src/tables.cpp
    src/transform.cpp
    src/entropy.cpp
    src/codec.cpp
)
target_include_directories(nxvc_ref PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(nxvc_ref PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(nxvc_ref PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(nxvc_ref PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

option(NXVC_FUZZ "Build the libFuzzer targets" OFF)

if(NXWARP_BUILD_TOOLS)
    foreach(t nxv-enc nxv-dec nxv-info)
        add_executable(${t} tools/${t}.cpp)
        target_link_libraries(${t} PRIVATE nxvc_ref)
        target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        set_target_properties(${t} PROPERTIES CXX_STANDARD 20)
    endforeach()
endif()
