# The sources are compiled once, as position-independent objects, and linked
# into both a static and a shared library.  The Python bindings load the
# shared one over ctypes (tests/python probes for the target nxvc_ref_shared);
# everything inside the project links the static one.
add_library(nxvc_ref_obj OBJECT
    src/tables.cpp
    src/transform.cpp
    src/entropy.cpp
    src/codec.cpp
)
target_include_directories(nxvc_ref_obj PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(nxvc_ref_obj PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(nxvc_ref_obj PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(nxvc_ref_obj PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_library(nxvc_ref STATIC $<TARGET_OBJECTS:nxvc_ref_obj>)
target_include_directories(nxvc_ref PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(nxvc_ref PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(nxvc_ref PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

add_library(nxvc_ref_shared SHARED $<TARGET_OBJECTS:nxvc_ref_obj>)
target_include_directories(nxvc_ref_shared PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(nxvc_ref_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(nxvc_ref_shared PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME nxvc_ref)

# The reference CLIs are part of the ref deliverable (the quality harness
# depends on nxv-enc / nxv-dec / nxv-info) and build with the library.
if(NOT DEFINED NXWARP_BUILD_TOOLS OR NXWARP_BUILD_TOOLS)
    foreach(t nxv-enc nxv-dec nxv-info)
        add_executable(${t} tools/${t}.cpp)
        target_link_libraries(${t} PRIVATE nxvc_ref)
        target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        set_target_properties(${t} PROPERTIES CXX_STANDARD 20)
    endforeach()
endif()
