# fov/ is listed after rc/ in the root component list, so nxvc_fov may not be
# defined yet.  target_link_libraries resolves at generate time; the include
# directory is added explicitly so the ordering does not matter.
if(NOT NXWARP_BUILD_FOV)
  message(FATAL_ERROR "nxvc_rc needs the fov/ component (NXWARP_BUILD_FOV=ON)")
endif()

add_library(nxvc_rc STATIC
    src/classify.cpp
    src/allocate.cpp
    src/governor.cpp
    src/tvm.cpp
    src/refresh.cpp
    src/synth.cpp
)
target_include_directories(nxvc_rc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(nxvc_rc PUBLIC ${CMAKE_SOURCE_DIR}/fov/include)
target_link_libraries(nxvc_rc PUBLIC nxvc_fov)
set_target_properties(nxvc_rc PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(nxvc_rc PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(NXWARP_BUILD_TOOLS)
  add_executable(nxvc-rcsim sim/rcsim.cpp sim/scene.cpp sim/render.cpp sim/temporal.cpp)
  target_link_libraries(nxvc-rcsim PRIVATE nxvc_rc)
  target_include_directories(nxvc-rcsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sim)
  set_target_properties(nxvc-rcsim PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  if(NOT MSVC)
    target_compile_options(nxvc-rcsim PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endif()
