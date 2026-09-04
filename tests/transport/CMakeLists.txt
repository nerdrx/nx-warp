# Tests for nxvc_transport.  Registered with ctest as transport.<name>.
if(NOT TARGET nxvc_transport)
    return()
endif()

foreach(t wire fec packetizer shadow receiver)
    add_executable(transport_test_${t} test_${t}.cpp)
    target_link_libraries(transport_test_${t} PRIVATE nxvc_transport)
    target_include_directories(transport_test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(transport_test_${t} PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    if(NOT MSVC)
        target_compile_options(transport_test_${t} PRIVATE -Wall -Wextra
            -Wno-unused-parameter)
    endif()
    add_test(NAME transport.${t} COMMAND transport_test_${t})
endforeach()
