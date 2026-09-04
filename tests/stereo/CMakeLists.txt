# tests/stereo - ctest targets for the STEREO experiment (docs/STEREO.md).
foreach(t raster disparity determinism)
    add_executable(stereo_test_${t} test_${t}.cpp)
    target_link_libraries(stereo_test_${t} PRIVATE nxs_sim)
    set_target_properties(stereo_test_${t} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    if(NOT MSVC)
        target_compile_options(stereo_test_${t} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME stereo.${t} COMMAND stereo_test_${t})
endforeach()

# The sim binary must at least run end to end on a small frame.
add_test(NAME stereo.sim_smoke
         COMMAND nxvc-stereosim --scene room --w 128 --h 128 --q 8 --range 1 --quiet)
