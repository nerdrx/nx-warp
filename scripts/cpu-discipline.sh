# scripts/cpu-discipline.sh -- the shared CPU-discipline prefix builder.
#
# Source this and call `nx_cpu_prefix [cpu-spec]`; it sets the array NICE to
# the longest prefix this machine actually supports:
#
#   chrt -i 0 taskset -c <spec> nice -n 19
#
# Each of the three is probed rather than assumed. The taskset probe is the
# one that matters: a fixed slice like `20-23` is right on the 32-core
# development host and is "Invalid argument" on a 2- or 4-core CI runner,
# where an unprobed taskset takes the whole script down. Where the slice does
# not exist we simply do not pin -- the idle class and the nice level, which
# are what actually keep a build out of the compositor's way, still apply.
#
# Override the slice with NX_CPUS, or set NX_NO_CPU_LIMIT=1 to drop the
# prefix entirely.

nx_cpu_prefix() {
    local spec="${NX_CPUS:-${1:-}}"
    NICE=()
    [ -n "${NX_NO_CPU_LIMIT:-}" ] && return 0
    if command -v chrt >/dev/null 2>&1 && chrt -i 0 true >/dev/null 2>&1; then
        NICE+=(chrt -i 0)
    fi
    if [ -n "$spec" ] && command -v taskset >/dev/null 2>&1 \
       && taskset -c "$spec" true >/dev/null 2>&1; then
        NICE+=(taskset -c "$spec")
    fi
    command -v nice >/dev/null 2>&1 && NICE+=(nice -n 19)
    return 0
}
