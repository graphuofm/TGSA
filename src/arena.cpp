#include "barry/arena.hpp"
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace barry {

static void* try_mmap(size_t bytes, bool huge) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    if (huge) flags |= MAP_HUGETLB;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    return p;
}

Arena::Arena(size_t reserve, bool huge_pages, bool transparent_huge)
    : base_(nullptr), reserved_(reserve), huge_pages_(huge_pages) {

    void* p = try_mmap(reserve, huge_pages_);
    if (p == MAP_FAILED && huge_pages_) {
        std::fprintf(stderr, "[Arena] huge pages mmap failed (%s), falling back to 4KB\n",
                     std::strerror(errno));
        huge_pages_ = false;
        p = try_mmap(reserve, false);
    }
    if (p == MAP_FAILED) {
        std::perror("[Arena] mmap");
        std::abort();
    }
    base_ = static_cast<char*>(p);
    // The arena is bump-allocated and every page that is reserved gets written,
    // so backing it with 2 MB pages costs no extra resident memory and removes
    // most of the TLB misses that a multi-gigabyte working set otherwise pays.
    bool thp = false;
#ifdef MADV_HUGEPAGE
    if (!huge_pages_ && transparent_huge)
        thp = (madvise(base_, reserved_, MADV_HUGEPAGE) == 0);
#else
    (void)transparent_huge;
#endif
    std::fprintf(stderr, "[Arena] reserved %.2f GB at %p (huge=%d thp=%d)\n",
                 reserve / 1e9, base_, huge_pages_ ? 1 : 0, thp ? 1 : 0);
}

Arena::~Arena() {
    if (base_) munmap(base_, reserved_);
}

BlockHeader* Arena::allocate_block(uint32_t capacity) {
    const size_t total = block_bytes(capacity);

    size_t off = bump_.fetch_add(total, std::memory_order_relaxed);
    if (off + total > reserved_) {
        // One line, not one per dropped edge: a full arena used to print per
        // failed insert and buried the run in tens of millions of identical
        // messages. The count is what matters and the engine reports it.
        if (num_failures_.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::fprintf(stderr,
                "[Arena] exhausted: %zu of %zu reserved bytes used; further "
                "appends will be refused (see Stats::edges_dropped)\n",
                off, reserved_);
        }
        return nullptr;
    }

    auto* h = reinterpret_cast<BlockHeader*>(base_ + off);
    h->count.store(0, std::memory_order_relaxed);
    h->capacity = capacity;
    h->next = nullptr;
    h->flags.store(0, std::memory_order_relaxed);

    num_blocks_.fetch_add(1, std::memory_order_relaxed);
    return h;
}

bool Arena::try_return_block(BlockHeader* block, uint32_t capacity) {
    const size_t total = block_bytes(capacity);
    const size_t off = static_cast<size_t>(reinterpret_cast<char*>(block) - base_);
    size_t expected = off + total;
    if (bump_.compare_exchange_strong(expected, off,
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        num_blocks_.fetch_sub(1, std::memory_order_relaxed);
        num_returned_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;   // someone allocated after us; the space stays spent
}

int32_t Arena::try_append(BlockHeader* block, const Edge& e,
                          const void* prop) {
    // Clear the published flag BEFORE bumping the count (same happens-before
    // argument as the bucket sorted flag: a reader that acquires the bumped
    // count sees this clear, so an observed flag proves a verified prefix).
    //
    // Only write when the flag is actually set. The ordering argument needs the
    // set-to-clear transition to carry release, and it still does; a flag that
    // is already clear cannot be observed as set, so the store would be pure
    // cache-line traffic. It is not free: on a time-ordered stream every writer
    // is appending to the same window, so this line is the most contended in
    // the engine.
    if (block->flags.load(std::memory_order_relaxed) != 0)
        block->flags.store(0, std::memory_order_release);
    uint32_t pos = block->count.fetch_add(1, std::memory_order_acq_rel);
    if (pos >= block->capacity) return -1;
    // Publish protocol: payload first, timestamp last with release. Fresh arena
    // pages are zero, so ts == 0 marks an unpublished slot; readers acquire-load
    // the timestamp and skip zeros, which makes queries correct DURING concurrent
    // ingestion instead of only between batches. Edges must carry ts > 0 (both
    // converters guarantee it; insert() drops ts == 0).
    Edge* slot = &edges_of(block)[pos];
    slot->src = e.src; slot->dst = e.dst; slot->weight = e.weight;
    // Sidecar bytes land before the timestamp's release store, so any reader
    // that observes the published edge also observes its properties.
    if (prop_base_ && prop)
        std::memcpy(prop_base_ + prop_slot(slot) * prop_bytes_, prop, prop_bytes_);
    __atomic_store_n(&slot->timestamp, e.timestamp, __ATOMIC_RELEASE);
    return static_cast<int32_t>(pos);
}

uint32_t Arena::try_append_run(BlockHeader* block, const Edge* edges,
                               uint32_t n, int32_t* first_off,
                               const uint8_t* props) {
    if (block->flags.load(std::memory_order_relaxed) != 0)
        block->flags.store(0, std::memory_order_release);

    // One bump for the whole run. Overshooting capacity is harmless: readers
    // clamp to min(count, capacity), exactly as the single-edge path already
    // relies on.
    uint32_t pos = block->count.fetch_add(n, std::memory_order_acq_rel);
    if (pos >= block->capacity) return 0;

    const uint32_t take = std::min(n, block->capacity - pos);
    Edge* arr = edges_of(block);
    for (uint32_t i = 0; i < take; ++i) {
        Edge* slot = &arr[pos + i];
        const Edge& e = edges[i];
        slot->src = e.src; slot->dst = e.dst; slot->weight = e.weight;
        if (prop_base_ && props)
            std::memcpy(prop_base_ + prop_slot(slot) * prop_bytes_,
                        props + static_cast<size_t>(i) * prop_bytes_,
                        prop_bytes_);
        // Same publication protocol as try_append: payload first, timestamp
        // last with release, and ts == 0 reserved for an unclaimed slot.
        __atomic_store_n(&slot->timestamp,
                         e.timestamp ? e.timestamp : uint64_t{1},
                         __ATOMIC_RELEASE);
    }
    *first_off = static_cast<int32_t>(pos);
    return take;
}

}  // namespace barry
