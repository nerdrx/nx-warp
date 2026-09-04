cmake_minimum_required(VERSION 3.22)
project(nxwarp_client_tests LANGUAGES CXX)

# Host build of the transport + frame-ring half of the client. Deliberately
# separate from android/CMakeLists.txt: this must build with nothing but a
# compiler, so the wire format can be checked on any machine.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(host-loopback
  host_loopback.cpp
  ../src/nxc_transport_stub.cpp
  ../src/nxc_frame_ring.cpp)

target_include_directories(host-loopback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(host-loopback PRIVATE -Wall -Wextra -Wno-unused-parameter)
