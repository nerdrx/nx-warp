# NX Warp -- predictor tests.

add_executable(nxvc_warp_test test_warp.cpp)
target_link_libraries(nxvc_warp_test PRIVATE nxvc_warp_ref)
target_include_directories(nxvc_warp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(nxvc_warp_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

add_executable(nxvc_warp_hash warp_hash.cpp)
target_link_libraries(nxvc_warp_hash PRIVATE nxvc_warp_ref)
target_include_directories(nxvc_warp_hash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(nxvc_warp_hash PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

foreach(suite int64 divide identity border mv corners interior oracle range filters)
    add_test(NAME warp.${suite} COMMAND nxvc_warp_test ${suite})
endforeach()
# The interior sweep and the oracle suite are the slow ones (2048^2 corpora).
set_tests_properties(warp.interior warp.oracle PROPERTIES TIMEOUT 900)

# --------------------------------------------------------------------------
# Determinism across optimisation levels and compilers.
# --------------------------------------------------------------------------
set(_cxx_list ${CMAKE_CXX_COMPILER})
find_program(NXWARP_GXX g++)
find_program(NXWARP_CLANGXX clang++)
foreach(c ${NXWARP_GXX} ${NXWARP_CLANGXX})
    if(c AND NOT c IN_LIST _cxx_list)
        list(APPEND _cxx_list ${c})
    endif()
endforeach()
string(REPLACE ";" "\\;" _cxx_escaped "${_cxx_list}")

add_test(NAME warp.determinism
         COMMAND ${CMAKE_COMMAND}
                 -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                 -DWARP=${CMAKE_SOURCE_DIR}/warp
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/determinism
                 -DCOMPILERS=${_cxx_escaped}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/determinism.cmake)
set_tests_properties(warp.determinism PROPERTIES TIMEOUT 1200)

# --------------------------------------------------------------------------
# GPU: the kernel must match the CPU reference on every pixel.
# Skips (exit 77) when no Vulkan ICD is installed.
# --------------------------------------------------------------------------
if(TARGET nxvc-warpdiff)
    add_test(NAME warp.gpu_diff COMMAND nxvc-warpdiff --tiles 10000 --all-devices)
    set_tests_properties(warp.gpu_diff PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()

# --------------------------------------------------------------------------
# warpsim smoke test: small resolution, few frames, just proves it runs and
# produces finite numbers. The documented 2048^2 run is driven by hand.
# --------------------------------------------------------------------------
if(TARGET nxvc-warpsim)
    add_test(NAME warp.sim_smoke
             COMMAND nxvc-warpsim --width 256 --height 256 --frames 12 --chain 8 --quiet)
    set_tests_properties(warp.sim_smoke PROPERTIES TIMEOUT 600)
endif()
