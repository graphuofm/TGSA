#pragma once
#include "edge.hpp"
#include "config.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace barry {

// kBlockPublished: every slot in [0, min(count,capacity)) carries a published
// (ts != 0) edge, verified by a quiescent finalize() pass. Readers may then take
// a no-check fast path over the block. The flag follows the same protocol as the
// bucket sorted flag: any append clears it (release) BEFORE bumping the count,
// so a reader that still observes the flag after an acquire count-load is
// looking at a fully verified prefix.
enum : uint32_t { kBlockPublished = 1u };

struct alignas(kCacheLineSize) BlockHeader {
    std::atomic<uint32_t> count;          // logical write index (may exceed capacity transiently)
    uint32_t              capacity;
    BlockHeader*          next;           // overflow chain
    std::atomic<uint32_t> flags;          // kBlockPublished
};
static_assert(sizeof(BlockHeader) <= kCacheLineSize,
              "BlockHeader must fit in one cache line");

inline Edge* edges_of(BlockHeader* h) {
    return reinterpret_cast<Edge*>(h + 1);
}
inline const Edge* edges_of(const BlockHeader* h) {
    return reinterpret_cast<const Edge*>(h + 1);
}

// Bump-allocator backed by an mmap'd virtual region.
class Arena {
public:
    Arena(size_t reserve_bytes, bool huge_pages = false,
          bool transparent_huge = true);
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    BlockHeader* allocate_block(uint32_t capacity = 16);

    // Give back a block that lost a CAS race, so the space is not burned.
    //
    // Speculative allocation is unavoidable here: a thread must have a block in
    // hand before it can try to publish it. What is avoidable is paying for the
    // losers. With N writers on a hot window, every fill event produced N-1
    // abandoned blocks, each twice the size of the last, and the bump pointer
    // ran roughly an order of magnitude ahead of the bytes actually written
    // until the reservation was exhausted. Returning the allocation succeeds
    // whenever nothing else has bumped since, which is the common case.
    bool try_return_block(BlockHeader* block, uint32_t capacity);

    size_t blocks_returned() const { return num_returned_.load(std::memory_order_relaxed); }
    size_t alloc_failures()  const { return num_failures_.load(std::memory_order_relaxed); }

    // Returns the slot offset on success, -1 if the block is full.
    int32_t try_append(BlockHeader* block, const Edge& e);

    // Claim up to `n` consecutive slots with a single counter bump and fill
    // them. Returns how many were placed (0 if the block was already full) and
    // writes the first slot's offset to *first_off.
    //
    // This exists because a time-ordered stream is the worst case for the
    // per-edge path: every writer is appending into the same window at the same
    // moment, so one counter and one cache line serialise the whole engine. The
    // measurement that showed it was the disorder sweep, where shuffling
    // arrivals made ingestion *faster* by spreading writers across windows.
    // Consecutive arrivals usually share a window, so one bump per run replaces
    // one per edge.
    uint32_t try_append_run(BlockHeader* block, const Edge* edges, uint32_t n,
                            int32_t* first_off);

    size_t bytes_used()       const { return bump_.load(std::memory_order_relaxed); }
    size_t bytes_reserved()   const { return reserved_; }
    size_t blocks_allocated() const { return num_blocks_.load(std::memory_order_relaxed); }
    bool   uses_huge_pages()  const { return huge_pages_; }

private:
    char*               base_;
    size_t              reserved_;
    std::atomic<size_t> bump_{0};
    std::atomic<size_t> num_blocks_{0};
    std::atomic<size_t> num_returned_{0};
    std::atomic<size_t> num_failures_{0};
    bool                huge_pages_;

    static size_t block_bytes(uint32_t capacity) {
        size_t total = sizeof(BlockHeader) + static_cast<size_t>(capacity) * sizeof(Edge);
        return (total + kCacheLineSize - 1) & ~(kCacheLineSize - 1);
    }
};

}  // namespace barry
