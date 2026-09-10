#pragma once
#include "arena.hpp"
#include <atomic>
#include <cstddef>

namespace barry {

// mmap-backed sparse pointer array. Empty slots = OS hasn't faulted in a page,
// so virtual size >> physical size for sparse temporal data.
class Timeline {
public:
    Timeline(size_t num_slots, bool huge_pages = false);
    ~Timeline();

    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    inline BlockHeader* load(uint64_t idx) const noexcept {
        return slots_[idx].load(std::memory_order_acquire);
    }

    inline bool cas(uint64_t idx, BlockHeader* expected, BlockHeader* desired) noexcept {
        return slots_[idx].compare_exchange_strong(
            expected, desired,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    size_t num_slots()      const noexcept { return num_slots_; }
    size_t bytes_virtual()  const noexcept { return bytes_; }
    bool   uses_huge_pages() const noexcept { return huge_pages_; }

    // Process-level RSS, useful for reporting physical footprint.
    static size_t process_rss_bytes();

private:
    std::atomic<BlockHeader*>* slots_;
    size_t num_slots_;
    size_t bytes_;
    bool   huge_pages_;
};

}  // namespace barry
