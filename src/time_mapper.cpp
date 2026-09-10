#include "barry/time_mapper.hpp"
#include <algorithm>
#include <cstdio>

namespace barry {

static uint32_t shift_for_window(uint64_t window_size) {
    if (window_size == 0) return 0;
    uint32_t shift = 0;
    while ((1ULL << shift) < window_size) ++shift;
    return shift;
}

uint32_t TimeMapper::auto_select_shift(
    const std::vector<uint64_t>& samples,
    uint64_t t_min,
    uint64_t t_max,
    size_t   target_avg) {
    if (samples.empty() || t_max <= t_min) return 10;
    return auto_select_shift_from_stats(samples.size(), t_min, t_max, target_avg);
}

uint32_t TimeMapper::auto_select_shift_from_stats(
    size_t   total_edges,
    uint64_t t_min,
    uint64_t t_max,
    size_t   target_avg) {
    if (total_edges == 0 || t_max <= t_min) return 10;
    if (target_avg == 0) target_avg = 64;

    uint64_t span = t_max - t_min;
    uint64_t desired_windows = std::max<uint64_t>(1, total_edges / target_avg);
    uint64_t window_size     = std::max<uint64_t>(1, span / desired_windows);

    uint32_t shift = shift_for_window(window_size);
    std::fprintf(stderr,
        "[TimeMapper] span=%lu edges=%zu target_avg=%zu shift=%u (window=%lu)\n",
        (unsigned long)span, total_edges, target_avg, shift, (unsigned long)(1ULL << shift));
    return shift;
}

}  // namespace barry
