#include "barry/engine.hpp"

#include <algorithm>
#include <chrono>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace barry {

namespace {

inline double now_sec() {
    using clk = std::chrono::high_resolution_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

inline size_t block_bytes(const BlockHeader* h) {
    size_t total = sizeof(BlockHeader) + static_cast<size_t>(h->capacity) * sizeof(Edge);
    return (total + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
}

}  // namespace

Barry::EvictStats Barry::evict_before(uint64_t t) {
    EvictStats st;
    st.rss_before = Timeline::process_rss_bytes();
    const double t0 = now_sec();

    if (t <= cfg_.t_min) { st.seconds = now_sec() - t0; st.rss_after = st.rss_before; return st; }

    // The cutoff is a timeline index, so finding what to drop is a shift, not a scan.
    uint64_t idx_cut = mapper_->map(t);
    idx_cut = std::min<uint64_t>(idx_cut, timeline_->num_slots());

    // Individual blocks are far smaller than a page, so releasing them one at a
    // time would free nothing. Collect the ranges, merge them, and hand the OS
    // whole pages.
    std::vector<std::pair<char*, size_t>> ranges;
    ranges.reserve(idx_cut / 4 + 16);

    for (uint64_t idx = 0; idx < idx_cut; ++idx) {
        BlockHeader* head = timeline_->load(idx);
        if (!head) continue;
        for (BlockHeader* b = head; b; b = b->next) {
            size_t nb = block_bytes(b);
            ranges.emplace_back(reinterpret_cast<char*>(b), nb);
            st.bytes_spanned += nb;
            ++st.blocks_released;
        }
        timeline_->cas(idx, head, nullptr);
        ++st.slots_cleared;
    }

    if (!ranges.empty()) {
        std::sort(ranges.begin(), ranges.end());
        const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));

        char* run_lo = ranges[0].first;
        char* run_hi = ranges[0].first + ranges[0].second;
        auto release = [&](char* lo, char* hi) {
            // Only pages fully inside the run may be dropped.
            uintptr_t a = (reinterpret_cast<uintptr_t>(lo) + page - 1) & ~(page - 1);
            uintptr_t b = reinterpret_cast<uintptr_t>(hi) & ~(page - 1);
            if (b > a) {
                if (::madvise(reinterpret_cast<void*>(a), b - a, MADV_DONTNEED) == 0)
                    st.bytes_released += (b - a);
            }
        };
        for (size_t i = 1; i < ranges.size(); ++i) {
            char* lo = ranges[i].first;
            char* hi = lo + ranges[i].second;
            if (lo <= run_hi) {                 // contiguous or overlapping
                run_hi = std::max(run_hi, hi);
            } else {
                release(run_lo, run_hi);
                run_lo = lo; run_hi = hi;
            }
        }
        release(run_lo, run_hi);
    }

    min_valid_time_ = t;
    st.seconds   = now_sec() - t0;
    st.rss_after = Timeline::process_rss_bytes();
    return st;
}

}  // namespace barry
