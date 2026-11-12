// A minimal check macro for the clip viewer's unit tests (plain main(), like the renderer's other tests).

#pragma once

#include <cmath>
#include <cstdio>

inline int& check_failures() {
    static int failures = 0;
    return failures;
}

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            check_failures()++;                                                                \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do {                                                                                       \
        const auto va_ = (a);                                                                  \
        const auto vb_ = (b);                                                                  \
        if (!(va_ == vb_)) {                                                                   \
            std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,   \
                        (long long)va_, (long long)vb_);                                       \
            check_failures()++;                                                                \
        }                                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tolerance)                                                            \
    do {                                                                                       \
        const double va_ = (a);                                                                \
        const double vb_ = (b);                                                                \
        if (!(std::fabs(va_ - vb_) <= (tolerance))) {                                          \
            std::printf("FAIL %s:%d: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            check_failures()++;                                                                \
        }                                                                                      \
    } while (0)

inline int check_result(const char* name) {
    if (check_failures() == 0)
        std::printf("%s: all checks pass\n", name);
    return check_failures() == 0 ? 0 : 1;
}
