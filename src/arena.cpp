#include "barry/arena.hpp"
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

namespace barry {

static void* try_mmap(size_t bytes, bool huge) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (huge) flags |= MAP_HUGETLB;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    return p;
}

Arena::Arena(size_t reserve, bool huge_pages)
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
    std::fprintf(stderr, "[Arena] reserved %.2f GB at %p (huge=%d)\n",
                 reserve / 1e9, base_, huge_pages_ ? 1 : 0);
}

Arena::~Arena() {
    if (base_) munmap(base_, reserved_);
}

BlockHeader* Arena::allocate_block(uint32_t capacity) {
    size_t total = sizeof(BlockHeader) + static_cast<size_t>(capacity) * sizeof(Edge);
    // Round up so the next block is also cache-line aligned.
    total = (total + kCacheLineSize - 1) & ~(kCacheLineSize - 1);

    size_t off = bump_.fetch_add(total, std::memory_order_relaxed);
    if (off + total > reserved_) {
        std::fprintf(stderr, "[Arena] OOM (used %zu + req %zu > reserved %zu)\n",
                     off, total, reserved_);
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

int32_t Arena::try_append(BlockHeader* block, const Edge& e) {
    // Clear the published flag BEFORE bumping the count (same happens-before
    // argument as the bucket sorted flag: a reader that acquires the bumped
    // count sees this clear, so an observed flag proves a verified prefix).
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
    __atomic_store_n(&slot->timestamp, e.timestamp, __ATOMIC_RELEASE);
    return static_cast<int32_t>(pos);
}

}  // namespace barry
