cmake_minimum_required(VERSION 3.25)
project(nxwarp VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Build hygiene, version discovery and instrumentation. These modules define
# no targets of their own beyond three INTERFACE ones (nxwarp::warnings,
# nxwarp::deterministic, nxwarp::version) and never reach into a component.
# See BUILDING.md; the presets in CMakePresets.json drive them.
include(nxwarp_options)
include(nxwarp_sanitizers)
include(nxwarp_version)

# Generated headers (nxvc/version.h) are on the include path for every
# component without any of them having to ask.
include_directories(${NXWARP_GENERATED_INCLUDE_DIR})

# Each component lives in its own directory with its own CMakeLists.txt and is
# built only when present, so the tree stays configurable while components land.
set(NXWARP_COMPONENTS ref warp transport rc fov stereo hybrid vk tools platform fuzz)
foreach(comp IN LISTS NXWARP_COMPONENTS)
  string(TOUPPER ${comp} COMP)
  if(comp STREQUAL "vk")
    option(NXWARP_BUILD_VK "Build the Vulkan encoder/decoder" OFF)
  else()
    option(NXWARP_BUILD_${COMP} "Build the ${comp} component" ON)
  endif()
endforeach()
option(NXWARP_BUILD_TESTS "Build tests" ON)

foreach(comp IN LISTS NXWARP_COMPONENTS)
  string(TOUPPER ${comp} COMP)
  if(NXWARP_BUILD_${COMP} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${comp}/CMakeLists.txt)
    add_subdirectory(${comp})
  endif()
endforeach()

# Tests: every tests/<name>/CMakeLists.txt is added directly, guarded on EXISTS,
# so a missing sibling never breaks configure. tests/CMakeLists.txt is NOT used.
if(NXWARP_BUILD_TESTS)
  enable_testing()
  set(_nxwarp_test_bindirs "")
  file(GLOB _nxwarp_test_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/tests/*)
  foreach(dir IN LISTS _nxwarp_test_dirs)
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
      get_filename_component(_name ${dir} NAME)
      if(_name STREQUAL "vk" OR _name MATCHES "^vk-")
        if(NOT NXWARP_BUILD_VK)
          continue()
        endif()
      endif()
      # A test dir named after a component is skipped when that component is off.
      string(TOUPPER ${_name} _NAME)
      if(DEFINED NXWARP_BUILD_${_NAME} AND NOT NXWARP_BUILD_${_NAME})
        continue()
      endif()
      add_subdirectory(${dir} ${CMAKE_CURRENT_BINARY_DIR}/tests/${_name})
      list(APPEND _nxwarp_test_bindirs ${CMAKE_CURRENT_BINARY_DIR}/tests/${_name})
    endif()
  endforeach()

  # --------------------------------------------------------- the stale-tool guard
  # A test that runs a tool binary -- `$<TARGET_FILE:nxvc-vkenc>`, or a target
  # named in a COMMAND -- gets the tool's PATH from CMake and no dependency on
  # it.  ctest builds nothing, so `ctest` after an edit happily runs yesterday's
  # binary and reports whatever that binary does.  It cost a false bug report
  # against main: `vk.encoder.inter.cv1088` "failed" for two runs because this
  # worktree's nxvc-vkenc predated the option the test passes it.  The failure
  # was the build, not the code, and nothing in the test graph could have said
  # so.  It is the same family as a shader compiled from a stale include: the
  # build system quietly decides the verdict.
  #
  # So one fixture, and every test requires it.  The fixture builds the default
  # target, which is a sub-second no-op when everything is current and is the
  # only formulation that cannot fall behind a new tool or a new test -- there
  # is no list of tools here to keep in step, which is the whole point.
  #
  # Applied from here rather than in each tests/*/CMakeLists.txt so that a test
  # directory added later is covered without anyone remembering to opt in.
  # `set_property(TEST ... DIRECTORY)` is what makes that possible and needs
  # CMake 3.28; below it the guard is skipped and says so, because a silent
  # downgrade is how this class of bug survives in the first place.
  add_test(NAME nxwarp.tools
           COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR})
  set_tests_properties(nxwarp.tools PROPERTIES
                       FIXTURES_SETUP nxwarp_tools TIMEOUT 3600)
  # Recursively, because most decoder tests live a level down --
  # tests/vk-decoder/{passA,passB,inter,unorm,toolmask} -- and a
  # directory's TESTS property lists only the tests added in that directory,
  # not in its children.  Attaching only the top level left every nested test
  # without the fixture, which the full suite hid (some other test pulls the
  # fixture in, and the fixture builds everything) and `ctest -R` on one nested
  # test exposes immediately: no build, "Unable to find executable".
  function(_nxwarp_attach_fixture _dir)
    get_property(_dt DIRECTORY ${_dir} PROPERTY TESTS)
    foreach(_t IN LISTS _dt)
      set_property(TEST ${_t} DIRECTORY ${_dir}
                   PROPERTY FIXTURES_REQUIRED nxwarp_tools)
    endforeach()
    get_property(_subs DIRECTORY ${_dir} PROPERTY SUBDIRECTORIES)
    foreach(_s IN LISTS _subs)
      _nxwarp_attach_fixture(${_s})
    endforeach()
  endfunction()
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    foreach(_bd IN LISTS _nxwarp_test_bindirs)
      _nxwarp_attach_fixture(${_bd})
    endforeach()
  else()
    message(STATUS
            "tests: CMake ${CMAKE_VERSION} < 3.28, the stale-tool fixture is "
            "NOT attached; run a build before ctest")
  endif()
endif()

option(NXWARP_BUILD_EXAMPLES "Build the examples in examples/" ON)
if(NXWARP_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt)
  add_subdirectory(examples)
endif()

# Install rules, the find_package(nxvc) package, pkg-config and CPack. These
# run last because they discover which component targets actually exist rather
# than being told; a component that has adopted the export convention (see
# RELEASE.md) is picked up automatically.
include(nxwarp_install)
include(nxwarp_cpack)

message(STATUS "--- nxwarp ---")
nxwarp_version_summary()
nxwarp_options_summary()
nxwarp_sanitizers_summary()
