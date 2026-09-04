cmake_minimum_required(VERSION 3.22)
project(nxvc_blast LANGUAGES CXX)

# Standalone by design: the blaster must build on a host with no Android SDK,
# no NDK and no Vulkan, because it is the half of the self test that runs on
# whatever machine is plugged into the headset.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(nxvc-blast main.cpp)
target_compile_options(nxvc-blast PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(NOT UNIX OR APPLE)
  message(WARNING
    "nxvc-blast uses sendmmsg/recvmmsg and is Linux only. It will not build here.")
endif()
