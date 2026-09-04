// Minimal assertion harness for the rate-control tests.
// SPDX-License-Identifier: Apache-2.0
#ifndef NXRC_TEST_UTIL_H
#define NXRC_TEST_UTIL_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace rct {

inline int  g_fail = 0;
inline int  g_checks = 0;
inline const char* g_case = "";

inline void begin(const char* name) { g_case = name; }

inline void fail(const char* file, int line, const std::string& msg) {
    ++g_fail;
    std::fprintf(stderr, "FAIL %s:%d [%s] %s\n", file, line, g_case, msg.c_str());
}

inline void check(bool ok, const char* file, int line, const std::string& msg) {
    ++g_checks;
    if (!ok) fail(file, line, msg);
}

inline int finish(const char* suite) {
    std::printf("%s: %d checks, %d failures\n", suite, g_checks, g_fail);
    return g_fail ? 1 : 0;
}

} // namespace rct

#define CHECK(cond) \
    rct::check((cond), __FILE__, __LINE__, "CHECK(" #cond ")")

#define CHECK_MSG(cond, msg) \
    rct::check((cond), __FILE__, __LINE__, std::string("CHECK(" #cond "): ") + (msg))

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        auto _a = (a); auto _b = (b);                                         \
        rct::check(_a == _b, __FILE__, __LINE__,                              \
                   std::string(#a " == " #b " (") + std::to_string(_a) +      \
                   " vs " + std::to_string(_b) + ")");                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                 \
    do {                                                                      \
        double _a = double(a), _b = double(b);                                \
        rct::check(std::fabs(_a - _b) <= double(tol), __FILE__, __LINE__,     \
                   std::string(#a " ~= " #b " (") + std::to_string(_a) +      \
                   " vs " + std::to_string(_b) + ")");                        \
    } while (0)

#define CHECK_LE(a, b)                                                        \
    do {                                                                      \
        double _a = double(a), _b = double(b);                                \
        rct::check(_a <= _b, __FILE__, __LINE__,                              \
                   std::string(#a " <= " #b " (") + std::to_string(_a) +      \
                   " vs " + std::to_string(_b) + ")");                        \
    } while (0)

#define CHECK_LT(a, b)                                                        \
    do {                                                                      \
        double _a = double(a), _b = double(b);                                \
        rct::check(_a < _b, __FILE__, __LINE__,                               \
                   std::string(#a " < " #b " (") + std::to_string(_a) +       \
                   " vs " + std::to_string(_b) + ")");                        \
    } while (0)

#endif
