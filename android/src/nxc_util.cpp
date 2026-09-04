#include "nxc_config.h"

#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace nxc {

uint64_t now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

namespace {
constexpr const char* kTag = "nxwarp";

void vlog(int prio, const char* fmt, va_list ap) {
    __android_log_vprint(prio, kTag, fmt, ap);
}
}  // namespace

void log_info(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(ANDROID_LOG_INFO, fmt, ap); va_end(ap);
}
void log_warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(ANDROID_LOG_WARN, fmt, ap); va_end(ap);
}
void log_err(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(ANDROID_LOG_ERROR, fmt, ap); va_end(ap);
}

}  // namespace nxc
