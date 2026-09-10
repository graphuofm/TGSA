#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

// Tiny header-only test harness. Avoids pulling in GoogleTest so we keep
// builds offline-friendly on the lab server.

#define EXPECT_TRUE(cond) do {                                              \
    if (!(cond)) {                                                          \
        std::fprintf(stderr, "FAIL %s:%d  EXPECT_TRUE(%s)\n",               \
                     __FILE__, __LINE__, #cond);                            \
        std::exit(1);                                                       \
    }                                                                       \
} while (0)

#define EXPECT_EQ(a, b) do {                                                \
    auto _va = (a); auto _vb = (b);                                         \
    if (!(_va == _vb)) {                                                    \
        std::fprintf(stderr, "FAIL %s:%d  EXPECT_EQ(%s, %s)\n",             \
                     __FILE__, __LINE__, #a, #b);                           \
        std::exit(1);                                                       \
    }                                                                       \
} while (0)

#define RUN(name) do {                                                      \
    std::fprintf(stderr, "  [run]  %s\n", #name);                           \
    name();                                                                 \
    std::fprintf(stderr, "  [pass] %s\n", #name);                           \
} while (0)
