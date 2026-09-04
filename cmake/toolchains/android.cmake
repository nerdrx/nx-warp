# Cross toolchain: Android arm64-v8a, a thin wrapper over the NDK's own file.
#
#   cmake --preset android-ndk
#
# The NDK ships a perfectly good toolchain file; wrapping it buys three things
# a preset cannot express on its own:
#
#   * finding the NDK without every developer exporting ANDROID_NDK_ROOT,
#   * pinning the ABI, API level and STL in one place instead of four presets,
#   * a readable error when the NDK is missing, rather than a compiler-not-
#     found failure forty lines deep.
#
# NDK location, in order:
#   1. -DANDROID_NDK=/path
#   2. $ANDROID_NDK_ROOT / $ANDROID_NDK_HOME / $ANDROID_NDK
#   3. the newest ndk/<version> under $ANDROID_SDK_ROOT or $ANDROID_HOME
#   4. the newest ndk/<version> under the dev box SDK
#
# Target: arm64-v8a, API 29 (the Pico 4 / Quest floor -- Android 10, and the
# level at which AHardwareBuffer's Vulkan external-memory import is reliable),
# c++_static so the .so has no libc++_shared.so to ship.
#
# Vulkan is ON here: the NDK carries the headers and libvulkan.so, and the
# whole point of an Android build is the compute decoder.  Tests are off --
# ctest cannot run on the host for an arm64 target.

set(NXWARP_ANDROID_SDK_FALLBACK "/run/media/nerdrx/Lex/claude/tools/android-sdk")

function(_nxwarp_newest_ndk sdk out)
    set(${out} "" PARENT_SCOPE)
    if(NOT IS_DIRECTORY "${sdk}/ndk")
        return()
    endif()
    file(GLOB _versions RELATIVE "${sdk}/ndk" "${sdk}/ndk/*")
    set(_best "")
    foreach(_v IN LISTS _versions)
        if(IS_DIRECTORY "${sdk}/ndk/${_v}" AND EXISTS "${sdk}/ndk/${_v}/build/cmake/android.toolchain.cmake")
            if(_best STREQUAL "" OR _v VERSION_GREATER _best)
                set(_best "${_v}")
            endif()
        endif()
    endforeach()
    if(_best)
        set(${out} "${sdk}/ndk/${_best}" PARENT_SCOPE)
    endif()
endfunction()

if(NOT DEFINED ANDROID_NDK)
    foreach(_env ANDROID_NDK_ROOT ANDROID_NDK_HOME ANDROID_NDK)
        if(NOT DEFINED ANDROID_NDK AND DEFINED ENV{${_env}} AND
           EXISTS "$ENV{${_env}}/build/cmake/android.toolchain.cmake")
            set(ANDROID_NDK "$ENV{${_env}}")
        endif()
    endforeach()
endif()

if(NOT DEFINED ANDROID_NDK)
    foreach(_sdk "$ENV{ANDROID_SDK_ROOT}" "$ENV{ANDROID_HOME}" "${NXWARP_ANDROID_SDK_FALLBACK}")
        if(NOT DEFINED ANDROID_NDK AND _sdk)
            _nxwarp_newest_ndk("${_sdk}" _found)
            if(_found)
                set(ANDROID_NDK "${_found}")
            endif()
        endif()
    endforeach()
endif()

if(NOT DEFINED ANDROID_NDK OR NOT EXISTS "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    message(FATAL_ERROR
        "Android NDK not found.  Pass -DANDROID_NDK=/path/to/ndk/<version>, or set\n"
        "ANDROID_NDK_ROOT, or install an NDK under \$ANDROID_SDK_ROOT/ndk/.\n"
        "Tried: ${ANDROID_NDK}")
endif()

message(STATUS "Android NDK: ${ANDROID_NDK}")

set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI")
set(ANDROID_PLATFORM "android-29" CACHE STRING "Minimum Android API level")
set(ANDROID_STL "c++_static" CACHE STRING "Android STL")
set(ANDROID_ARM_NEON ON)

# The NDK toolchain file does the actual work.
include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
