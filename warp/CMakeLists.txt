# NX Warp -- the pose-warped predictor module.
#
# nxvc_warp_ref is the portable, bit-exact CPU library: C++20, no POSIX
# headers, no platform assumptions, builds under mingw-w64 with
# NXWARP_BUILD_VK=OFF. Everything Vulkan lives behind NXWARP_WARP_VK_TOOLS.

add_library(nxvc_warp_ref STATIC
    ref/warp_ref.cpp
    ref/homography.cpp
    ref/warp_oracle.cpp
)
target_include_directories(nxvc_warp_ref PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(nxvc_warp_ref PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(nxvc_warp_ref PRIVATE -Wall -Wextra)
endif()

# --------------------------------------------------------------------------
# Shader: compiled to SPIR-V at build time when a compiler is available.
#
# Through the tree's one shader-build rule (vk/common/cmake/nxvc_gen_spv.cmake)
# when glslc is present: -O0 plus spirv-opt with the Adreno-safe pass list.
# This used to be a bare `glslc -O`, whose built-in pass list contains the
# redundancy-elimination pass the Adreno 650 driver miscompiles; the predictor
# runs on the headset, so it is squarely in scope.  See docs/ADRENO-RULES.md.
#
# The glslangValidator fallback stays unoptimised: it has no equivalent pass
# control, and unoptimised SPIR-V is always safe.
# --------------------------------------------------------------------------
find_program(NXWARP_GLSLC NAMES glslc)
find_program(NXWARP_GLSLANG NAMES glslangValidator)

set(NXWARP_GEN_SPV ${CMAKE_CURRENT_SOURCE_DIR}/../vk/common/cmake/nxvc_gen_spv.cmake)
set(NXWARP_SPV_PASSES_CMAKE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vk/common/cmake/NxvcShaderPasses.cmake)

set(NXWARP_WARP_SPV "")
if(NXWARP_GLSLC OR NXWARP_GLSLANG)
    set(NXWARP_WARP_SPV ${CMAKE_CURRENT_BINARY_DIR}/warp_tile.comp.spv)
    set(_spv_deps ${CMAKE_CURRENT_SOURCE_DIR}/glsl/warp_tile.comp)
    if(NXWARP_GLSLC)
        set(_cmd ${CMAKE_COMMAND}
                 -DGLSLC=${NXWARP_GLSLC}
                 -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/glsl/warp_tile.comp
                 -DOUT=${NXWARP_WARP_SPV}
                 -DNAME=warp_tile
                 -DSTYLE=raw
                 -P ${NXWARP_GEN_SPV})
        list(APPEND _spv_deps ${NXWARP_GEN_SPV} ${NXWARP_SPV_PASSES_CMAKE})
    else()
        set(_cmd ${NXWARP_GLSLANG} -V --target-env vulkan1.1 -S comp
                 ${CMAKE_CURRENT_SOURCE_DIR}/glsl/warp_tile.comp -o ${NXWARP_WARP_SPV})
    endif()
    add_custom_command(OUTPUT ${NXWARP_WARP_SPV}
        COMMAND ${_cmd}
        DEPENDS ${_spv_deps}
        COMMENT "glsl: warp_tile.comp -> SPIR-V"
        VERBATIM)
    add_custom_target(nxvc_warp_spv ALL DEPENDS ${NXWARP_WARP_SPV})
endif()

# --------------------------------------------------------------------------
# Tools
# --------------------------------------------------------------------------
if(NOT DEFINED NXWARP_BUILD_TOOLS)
    set(NXWARP_BUILD_TOOLS ON)
endif()

if(NXWARP_BUILD_TOOLS)
    add_executable(nxvc-warpsim tools/warpsim/main.cpp)
    target_link_libraries(nxvc-warpsim PRIVATE nxvc_warp_ref)
    set_target_properties(nxvc-warpsim PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

    option(NXWARP_WARP_VK_TOOLS "Build the Vulkan warp/CPU diff harness" ON)
    if(NXWARP_WARP_VK_TOOLS)
        # Minimal, self-contained Vulkan setup. vk/common/ is being built in
        # parallel by another agent; this harness deliberately does not depend
        # on it yet and is expected to be swapped over later.
        find_package(Vulkan QUIET)
        if(NOT Vulkan_FOUND)
            find_path(NXWARP_VK_INCLUDE vulkan/vulkan.h
                      PATHS /usr/include
                            ${CMAKE_SOURCE_DIR}/../tools/local/include)
            find_library(NXWARP_VK_LIB NAMES vulkan)
        endif()
        if(Vulkan_FOUND OR (NXWARP_VK_INCLUDE AND NXWARP_VK_LIB))
            add_executable(nxvc-warpdiff tools/warpdiff/main.cpp)
            target_link_libraries(nxvc-warpdiff PRIVATE nxvc_warp_ref)
            set_target_properties(nxvc-warpdiff PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
            if(Vulkan_FOUND)
                target_link_libraries(nxvc-warpdiff PRIVATE Vulkan::Vulkan)
            else()
                target_include_directories(nxvc-warpdiff PRIVATE ${NXWARP_VK_INCLUDE})
                target_link_libraries(nxvc-warpdiff PRIVATE ${NXWARP_VK_LIB})
            endif()
            if(NXWARP_WARP_SPV)
                add_dependencies(nxvc-warpdiff nxvc_warp_spv)
                target_compile_definitions(nxvc-warpdiff
                    PRIVATE NXVC_WARP_SPV_PATH="${NXWARP_WARP_SPV}")
            endif()
            set(NXWARP_HAVE_WARPDIFF ON CACHE INTERNAL "")
        else()
            message(STATUS "warp: Vulkan not found, nxvc-warpdiff disabled")
        endif()
    endif()
endif()
