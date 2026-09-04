// Minimal dependency-free test harness for the nxvc_transport tests.
#ifndef NXVC_TESTS_TRANSPORT_UTIL_H
#define NXVC_TESTS_TRANSPORT_UTIL_H

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tt {

inline int g_failures = 0;
inline int g_checks = 0;
inline std::string g_case;

inline void begin(const char* name) {
    g_case = name;
    std::printf("  %-46s", name);
    std::fflush(stdout);
}
inline void end() {
    static int last = 0;
    std::printf("%s\n", g_failures == last ? "ok" : "FAILED");
    last = g_failures;
}

#define TT_CHECK(cond)                                                              \
    do {                                                                            \
        ++::tt::g_checks;                                                           \
        if (!(cond)) {                                                              \
            ++::tt::g_failures;                                                     \
            std::printf("\n    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,      \
                        #cond);                                                     \
        }                                                                           \
    } while (0)

#define TT_EQ(a, b)                                                                 \
    do {                                                                            \
        ++::tt::g_checks;                                                           \
        auto _a = (a);                                                              \
        auto _b = (b);                                                              \
        if (!(_a == _b)) {                                                          \
            ++::tt::g_failures;                                                     \
            std::printf("\n    %s:%d: %s == %s failed (%lld vs %lld)\n", __FILE__,  \
                        __LINE__, #a, #b, (long long)(_a), (long long)(_b));        \
        }                                                                           \
    } while (0)

inline int report(const char* suite) {
    std::printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// Small deterministic RNG so failures reproduce from the seed alone.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t u32(uint32_t n) { return n ? uint32_t(next() % n) : 0; }
    double u01() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
};

}  // namespace tt

#endif
