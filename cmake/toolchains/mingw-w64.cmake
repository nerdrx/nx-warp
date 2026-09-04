# Cross toolchain: Windows x86_64 via llvm-mingw.
#
#   cmake --preset mingw-w64
#
# Which llvm-mingw is used, in order:
#   1. -DLLVM_MINGW_ROOT=/path on the command line
#   2. $LLVM_MINGW_ROOT in the environment
#   3. an x86_64-w64-mingw32-clang already on PATH (a distro package works too)
#   4. /run/media/nerdrx/Lex/claude/tools/llvm-mingw -- the dev box default,
#      the same tree the WiVRn NX Windows port uses
#
# The toolchain's bin/ is prepended to PATH for this configure so the driver
# finds its own lld, llvm-ar and windres without a wrapper script; no shell
# setup is needed before `cmake --preset mingw-w64`.
#
# Only the CPU components cross-compile.  The preset sets NXWARP_BUILD_VK=OFF
# and NXWARP_BUILD_TESTS=OFF: this is a portability check, and there is no
# Windows machine to run ctest on from here.
#
# GCC-flavour mingw (Debian's g++-mingw-w64-x86-64, what CI installs) also
# works: point LLVM_MINGW_ROOT at nothing and set
#   -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix
#   -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix
# The *posix* flavour is mandatory -- win32 has no std::thread.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(NXWARP_MINGW_TRIPLE x86_64-w64-mingw32)

if(NOT DEFINED LLVM_MINGW_ROOT)
    if(DEFINED ENV{LLVM_MINGW_ROOT})
        set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
    else()
        find_program(_nxwarp_mingw_on_path ${NXWARP_MINGW_TRIPLE}-clang)
        if(_nxwarp_mingw_on_path)
            get_filename_component(_nxwarp_mingw_bin "${_nxwarp_mingw_on_path}" DIRECTORY)
            get_filename_component(LLVM_MINGW_ROOT "${_nxwarp_mingw_bin}" DIRECTORY)
        else()
            set(LLVM_MINGW_ROOT "/run/media/nerdrx/Lex/claude/tools/llvm-mingw")
        endif()
    endif()
endif()

if(NOT EXISTS "${LLVM_MINGW_ROOT}/bin/${NXWARP_MINGW_TRIPLE}-clang")
    message(FATAL_ERROR
        "llvm-mingw not found under '${LLVM_MINGW_ROOT}'.\n"
        "Unpack a release from https://github.com/mstorsjo/llvm-mingw and pass\n"
        "  -DLLVM_MINGW_ROOT=/path/to/llvm-mingw\n"
        "or set LLVM_MINGW_ROOT in the environment.")
endif()

# So the compiler driver's siblings resolve without a login shell.
set(ENV{PATH} "${LLVM_MINGW_ROOT}/bin:$ENV{PATH}")

set(CMAKE_C_COMPILER   "${LLVM_MINGW_ROOT}/bin/${NXWARP_MINGW_TRIPLE}-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/${NXWARP_MINGW_TRIPLE}-clang++")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_ROOT}/bin/${NXWARP_MINGW_TRIPLE}-windres")
set(CMAKE_AR           "${LLVM_MINGW_ROOT}/bin/llvm-ar")
set(CMAKE_RANLIB       "${LLVM_MINGW_ROOT}/bin/llvm-ranlib")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/${NXWARP_MINGW_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Self-contained binaries: no runtime DLLs to ship next to nxv-enc.exe.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
