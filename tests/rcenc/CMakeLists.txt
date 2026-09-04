# The rate-control driver needs both libraries; skip cleanly when either
# component is off.
if(NOT TARGET nxvc_rcenc)
  return()
endif()

add_executable(rcenc_test_encdrive test_encdrive.cpp)
target_link_libraries(rcenc_test_encdrive PRIVATE nxvc_rcenc)
target_include_directories(rcenc_test_encdrive PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/rc)
set_target_properties(rcenc_test_encdrive PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(rcenc_test_encdrive PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME rcenc.encdrive COMMAND rcenc_test_encdrive)
