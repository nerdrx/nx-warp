#!/usr/bin/env bash
# bootstrap.sh -- report what this repository needs and what this machine has.
#
# It installs nothing. There is no sudo in this file and there will not be one:
# the distributions in play (Arch, Debian/Ubuntu, the CI image, the Android SDK
# on a separate volume) disagree about package names, and a script that guesses
# and then runs a privileged package manager is a script that eventually
# breaks someone's machine. It prints a table and, at the end, the exact
# command to run for the packages that are missing.
#
#   scripts/bootstrap.sh              full table
#   scripts/bootstrap.sh --quiet      only what is missing
#   scripts/bootstrap.sh --android    include the Android SDK/NDK/JDK rows
#   scripts/bootstrap.sh --strict     exit non-zero if a REQUIRED tool is absent
#
# Exit status is 0 unless --strict is given and something required is missing.

set -uo pipefail

QUIET=0
WANT_ANDROID=0
STRICT=0
for arg in "$@"; do
    case "$arg" in
        --quiet|-q)   QUIET=1 ;;
        --android)    WANT_ANDROID=1 ;;
        --strict)     STRICT=1 ;;
        --help|-h)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_BAD=$'\033[31m'
    C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
    C_OK=''; C_WARN=''; C_BAD=''; C_DIM=''; C_OFF=''
fi

MISSING_REQUIRED=()
MISSING_OPTIONAL=()
ROWS=()

# row <status> <tool> <found version or -> <needed by>
row() {
    ROWS+=("$1"$'\t'"$2"$'\t'"$3"$'\t'"$4")
}

# check <required|optional> <name> <needed-by> <command> [version-command]
check() {
    local kind="$1" name="$2" needed="$3" cmd="$4" vercmd="${5:-}"
    local path version
    if path="$(command -v "$cmd" 2>/dev/null)"; then
        if [ -n "$vercmd" ]; then
            version="$(eval "$vercmd" 2>/dev/null | head -n1)"
        else
            version="$path"
        fi
        row OK "$name" "${version:-found}" "$needed"
        return 0
    fi
    if [ "$kind" = required ]; then
        row MISSING "$name" "-" "$needed"
        MISSING_REQUIRED+=("$name")
    else
        row OPTIONAL "$name" "-" "$needed"
        MISSING_OPTIONAL+=("$name")
    fi
    return 1
}

# ---------------------------------------------------------------------------
# Build toolchain
# ---------------------------------------------------------------------------
CMAKE_MIN=3.25
if command -v cmake >/dev/null 2>&1; then
    cmake_ver="$(cmake --version | head -n1 | awk '{print $3}')"
    if [ "$(printf '%s\n%s\n' "$CMAKE_MIN" "$cmake_ver" | sort -V | head -n1)" = "$CMAKE_MIN" ]; then
        row OK "cmake (>= $CMAKE_MIN)" "$cmake_ver" "everything C/C++"
    else
        row MISSING "cmake (>= $CMAKE_MIN)" "$cmake_ver too old" "everything C/C++"
        MISSING_REQUIRED+=("cmake >= $CMAKE_MIN")
    fi
else
    row MISSING "cmake (>= $CMAKE_MIN)" "-" "everything C/C++"
    MISSING_REQUIRED+=("cmake >= $CMAKE_MIN")
fi

check required "ninja"   "the generator every preset uses" ninja "ninja --version"

# A C++20 compiler: either one is enough, both is better (the bit-exactness
# rule is what two compilers are for).
have_cxx=0
if check optional "g++"     "C++20 host builds" g++ "g++ --version"; then have_cxx=1; fi
if check optional "clang++" "C++20 host builds, sanitizers, libFuzzer" clang++ "clang++ --version"; then have_cxx=1; fi
if [ "$have_cxx" -eq 0 ]; then
    MISSING_REQUIRED+=("a C++20 compiler (g++ or clang++)")
    row MISSING "a C++20 compiler" "-" "ref/, warp/, transport/, rc/, vk/"
fi

check optional "ccache"      "rebuild speed; picked up automatically" ccache "ccache --version"
check optional "clang-tidy"  "the advisory lint (.clang-tidy)" clang-tidy "clang-tidy --version 2>&1 | grep -im1 version"
check optional "clang-format" "the format check (>= 16)" clang-format "clang-format --version"
check optional "gcovr"       "coverage reports from the gcc coverage preset" gcovr "gcovr --version | head -1"
check optional "llvm-cov"    "coverage reports from the clang coverage preset" llvm-cov "llvm-cov --version 2>&1 | grep -im1 version"
check optional "pre-commit"  ".pre-commit-config.yaml hooks" pre-commit "pre-commit --version"

# ---------------------------------------------------------------------------
# Shaders
# ---------------------------------------------------------------------------
have_glsl=0
if check optional "glslangValidator" "GLSL -> SPIR-V 1.4" glslangValidator "glslangValidator --version | head -1"; then have_glsl=1; fi
if check optional "glslc" "GLSL -> SPIR-V (shaderc, the alternative)" glslc "glslc --version | head -1"; then have_glsl=1; fi
if [ "$have_glsl" -eq 0 ]; then
    MISSING_REQUIRED+=("glslang or glslc (needed by vk/, warp/, bench/)")
fi
check optional "spirv-val" "shader validation, run it on anything new" spirv-val "spirv-val --version | head -1"
check optional "spirv-opt" "shader optimisation" spirv-opt "spirv-opt --version | head -1"

# ---------------------------------------------------------------------------
# Vulkan
# ---------------------------------------------------------------------------
vk_header=""
for d in /usr/include /usr/local/include; do
    if [ -f "$d/vulkan/vulkan.h" ]; then vk_header="$d/vulkan/vulkan.h"; break; fi
done
if [ -n "$vk_header" ]; then
    row OK "Vulkan headers" "$vk_header" "vk/"
else
    row OPTIONAL "Vulkan headers" "-" "vk/ (NXWARP_BUILD_VK=ON)"
    MISSING_OPTIONAL+=("Vulkan headers")
fi

vk_loader=""
for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib; do
    for f in "$d"/libvulkan.so*; do
        if [ -e "$f" ]; then vk_loader="$f"; break 2; fi
    done
done
if [ -n "$vk_loader" ]; then
    row OK "Vulkan loader" "$vk_loader" "vk/"
else
    row OPTIONAL "Vulkan loader" "-" "vk/"
    MISSING_OPTIONAL+=("Vulkan loader (libvulkan)")
fi

# lavapipe: the software ICD CI runs on. Its subgroup size of 8 is why the
# cluster size is 8 (CONTRIBUTING.md), so this is not merely a fallback --
# it is the reference GPU for determinism.
lvp=""
for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d /etc/vulkan/icd.d "$HOME/.local/share/vulkan/icd.d"; do
    [ -d "$d" ] || continue
    found="$(find "$d" -maxdepth 1 -name 'lvp_icd*.json' 2>/dev/null | sort | head -n1)"
    if [ -n "$found" ]; then lvp="$found"; break; fi
done
if [ -n "$lvp" ]; then
    row OK "lavapipe ICD" "$lvp" "vk/ tests without a GPU"
else
    row OPTIONAL "lavapipe ICD" "-" "vk/ tests without a GPU"
    MISSING_OPTIONAL+=("lavapipe (mesa vulkan-swrast / mesa-vulkan-drivers)")
fi
check optional "vulkaninfo" "confirming which ICD you actually got" vulkaninfo

# ---------------------------------------------------------------------------
# Python quality harness
# ---------------------------------------------------------------------------
PY="${NXWARP_QUALITY_PYTHON:-python3}"
if command -v "$PY" >/dev/null 2>&1; then
    row OK "python3" "$("$PY" --version 2>&1)" "tools/quality/"
    for mod in numpy pytest; do
        if "$PY" -c "import $mod" >/dev/null 2>&1; then
            ver="$("$PY" -c "import $mod; print($mod.__version__)" 2>/dev/null)"
            row OK "python: $mod" "${ver:-present}" "tools/quality/"
        else
            row OPTIONAL "python: $mod" "-" "tools/quality/"
            MISSING_OPTIONAL+=("python $mod (for $PY)")
        fi
    done
else
    row MISSING "python3" "-" "tools/quality/"
    MISSING_REQUIRED+=("python3")
fi

# ---------------------------------------------------------------------------
# ffmpeg anchors
# ---------------------------------------------------------------------------
if command -v ffmpeg >/dev/null 2>&1; then
    row OK "ffmpeg" "$(ffmpeg -version 2>/dev/null | head -n1 | cut -c1-48)" "quality anchors"
    enc="$(ffmpeg -hide_banner -encoders 2>/dev/null || true)"
    for e in libx264 libx265; do
        if printf '%s' "$enc" | grep -q " $e "; then
            row OK "ffmpeg: $e" "present" "x264/x265 anchors"
        else
            row OPTIONAL "ffmpeg: $e" "-" "x264/x265 anchors"
            MISSING_OPTIONAL+=("ffmpeg built with $e")
        fi
    done
    if ffmpeg -hide_banner -filters 2>/dev/null | grep -qi vmaf; then
        row OK "ffmpeg: libvmaf" "present" "VMAF metric"
    else
        row OPTIONAL "ffmpeg: libvmaf" "-" "VMAF metric (skipped, not fatal)"
    fi
else
    row OPTIONAL "ffmpeg" "-" "quality anchors (optional)"
    MISSING_OPTIONAL+=("ffmpeg with libx264/libx265")
fi

# ---------------------------------------------------------------------------
# Android, for bench/ and the android-ndk preset
# ---------------------------------------------------------------------------
if [ "$WANT_ANDROID" -eq 1 ]; then
    sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/run/media/nerdrx/Lex/claude/tools/android-sdk}}"
    if [ -d "$sdk" ]; then
        row OK "Android SDK" "$sdk" "bench/, android-ndk preset"
    else
        row OPTIONAL "Android SDK" "-" "bench/, android-ndk preset"
        MISSING_OPTIONAL+=("Android SDK")
    fi

    ndk="${ANDROID_NDK_ROOT:-}"
    if [ -z "$ndk" ] && [ -d "$sdk/ndk" ]; then
        ndk="$(find "$sdk/ndk" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -n1)"
    fi
    if [ -n "$ndk" ] && [ -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
        row OK "Android NDK" "$ndk" "android-ndk preset"
    else
        row OPTIONAL "Android NDK" "-" "android-ndk preset"
        MISSING_OPTIONAL+=("Android NDK (r26+)")
    fi

    if command -v java >/dev/null 2>&1; then
        jver="$(java -version 2>&1 | head -n1)"
        row OK "JDK" "$jver" "bench/ gradle build"
        case "$jver" in
            *\"1[7-9]*|*\"2[0-9]*) ;;
            *) MISSING_OPTIONAL+=("JDK 17+ (found: $jver)") ;;
        esac
    else
        row OPTIONAL "JDK 17" "-" "bench/ gradle build"
        MISSING_OPTIONAL+=("JDK 17")
    fi

    check optional "adb" "installing the bench APK on the headset" adb "adb --version | head -1"
fi

check optional "git" "version stamping (git describe)" git "git --version"
check optional "gh"  "scripts/release.sh --publish" gh "gh --version | head -1"

# ---------------------------------------------------------------------------
# Print
# ---------------------------------------------------------------------------
paint() {
    case "$1" in
        OK)       printf '%s%-8s%s' "$C_OK" "ok" "$C_OFF" ;;
        OPTIONAL) printf '%s%-8s%s' "$C_WARN" "absent" "$C_OFF" ;;
        MISSING)  printf '%s%-8s%s' "$C_BAD" "MISSING" "$C_OFF" ;;
    esac
}

if [ "$QUIET" -eq 0 ]; then
    printf '\nNX Warp prerequisites\n\n'
    printf '%-8s  %-22s  %-40s  %s\n' "status" "tool" "found" "needed by"
    printf '%-8s  %-22s  %-40s  %s\n' "--------" "----------------------" \
        "----------------------------------------" "---------------------------"
    for r in "${ROWS[@]}"; do
        IFS=$'\t' read -r st name ver need <<<"$r"
        paint "$st"
        printf '  %-22s  %-40.40s  %s%s%s\n' "$name" "$ver" "$C_DIM" "$need" "$C_OFF"
    done
    printf '\n'
fi

if [ ${#MISSING_REQUIRED[@]} -gt 0 ]; then
    printf '%sRequired and missing:%s\n' "$C_BAD" "$C_OFF"
    printf '  - %s\n' "${MISSING_REQUIRED[@]}"
    printf '\n'
fi
if [ ${#MISSING_OPTIONAL[@]} -gt 0 ] && [ "$QUIET" -eq 0 ]; then
    printf '%sOptional and missing (features degrade, nothing breaks):%s\n' "$C_WARN" "$C_OFF"
    printf '  - %s\n' "${MISSING_OPTIONAL[@]}"
    printf '\n'
fi

if [ ${#MISSING_REQUIRED[@]} -gt 0 ] || [ ${#MISSING_OPTIONAL[@]} -gt 0 ]; then
    cat <<'EOF'
Install them yourself -- this script never runs a package manager.

  Arch:
    sudo pacman -S base-devel cmake ninja ccache clang glslang spirv-tools \
        vulkan-headers vulkan-icd-loader vulkan-swrast vulkan-tools \
        python python-numpy python-pytest ffmpeg gcovr

  Debian / Ubuntu:
    sudo apt install build-essential cmake ninja-build ccache clang \
        clang-format clang-tidy glslang-tools spirv-tools libvulkan-dev \
        vulkan-tools mesa-vulkan-drivers python3 python3-numpy \
        python3-pytest ffmpeg gcovr

  Android (bench/ and the android-ndk preset): install the SDK, NDK r26+ and
  JDK 17 through Android Studio or sdkmanager, then export ANDROID_SDK_ROOT.

EOF
fi

if [ ${#MISSING_REQUIRED[@]} -eq 0 ]; then
    printf '%sEverything required is present.%s  Next: scripts/build-all.sh dev\n' "$C_OK" "$C_OFF"
fi

if [ "$STRICT" -eq 1 ] && [ ${#MISSING_REQUIRED[@]} -gt 0 ]; then
    exit 1
fi
exit 0
