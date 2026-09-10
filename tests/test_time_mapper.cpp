#include "barry/time_mapper.hpp"
#include "test_common.hpp"

using namespace barry;

static void test_basic_mapping() {
    TimeMapper m(1000, 4);  // window = 16
    EXPECT_EQ(m.map(1000), 0u);
    EXPECT_EQ(m.map(1015), 0u);
    EXPECT_EQ(m.map(1016), 1u);
    EXPECT_EQ(m.map(1031), 1u);
    EXPECT_EQ(m.window_size(), 16u);
    EXPECT_EQ(m.unmap(2), 1032u);
}

static void test_zero_shift_identity() {
    TimeMapper m(0, 0);
    EXPECT_EQ(m.map(0), 0u);
    EXPECT_EQ(m.map(42), 42u);
    EXPECT_EQ(m.window_size(), 1u);
}

static void test_auto_select_dense() {
    // 1M edges over 1M-second span, target 64/window  -> ~16K windows
    // -> ~64 seconds per window -> shift around 6.
    uint32_t s = TimeMapper::auto_select_shift_from_stats(
        1'000'000, 0, 1'000'000, 64);
    EXPECT_TRUE(s >= 5 && s <= 8);
}

static void test_auto_select_sparse() {
    // 1K edges over 30 years (~1e9 s). target 64 -> ~16 windows -> ~6e7 s/win.
    // 2^25 ~ 3.3e7, 2^26 ~ 6.7e7  -> shift ~25-26.
    uint32_t s = TimeMapper::auto_select_shift_from_stats(
        1000, 0, 30ULL * 365 * 24 * 3600, 64);
    EXPECT_TRUE(s >= 24 && s <= 27);
}

static void test_auto_select_degenerate() {
    EXPECT_EQ(TimeMapper::auto_select_shift_from_stats(0, 0, 100), 10u);
    EXPECT_EQ(TimeMapper::auto_select_shift_from_stats(100, 100, 100), 10u);
}

int main() {
    RUN(test_basic_mapping);
    RUN(test_zero_shift_identity);
    RUN(test_auto_select_dense);
    RUN(test_auto_select_sparse);
    RUN(test_auto_select_degenerate);
    std::fprintf(stderr, "test_time_mapper OK\n");
    return 0;
}
