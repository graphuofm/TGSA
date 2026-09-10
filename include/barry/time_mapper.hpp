#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace barry {

// Algorithm 1: Temporal Address Mapping.
// index = (T - T_start) >> S
class TimeMapper {
public:
    TimeMapper(uint64_t t_start, uint32_t shift)
        : t_start_(t_start), shift_(shift) {}

    inline uint64_t map(uint64_t timestamp) const noexcept {
        return (timestamp - t_start_) >> shift_;
    }

    inline uint64_t unmap(uint64_t index) const noexcept {
        return t_start_ + (index << shift_);
    }

    inline uint64_t window_size() const noexcept {
        return 1ULL << shift_;
    }

    // Algorithm 2: Adaptive Granularity Selection.
    // Picks shift so that the average window holds ~target_avg edges.
    static uint32_t auto_select_shift(
        const std::vector<uint64_t>& sample_timestamps,
        uint64_t t_min,
        uint64_t t_max,
        size_t   target_avg_edges_per_window = 64);

    // Same as above but driven by aggregate stats (avoids materializing samples).
    static uint32_t auto_select_shift_from_stats(
        size_t   total_edges,
        uint64_t t_min,
        uint64_t t_max,
        size_t   target_avg_edges_per_window = 64);

    uint64_t t_start() const noexcept { return t_start_; }
    uint32_t shift()   const noexcept { return shift_; }

private:
    uint64_t t_start_;
    uint32_t shift_;
};

}  // namespace barry
