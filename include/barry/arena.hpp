#pragma once
#include "edge.hpp"
#include "config.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace barry {

struct alignas(kCacheLineSize) BlockHeader {
    std::atomic<uint32_t> count;          // logical write index (may exceed capacity transiently)
    uint32_t              capacity;
    BlockHeader*          next;           // overflow chain
    uint32_t              padding;
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
    Arena(size_t reserve_bytes, bool huge_pages = false);
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    BlockHeader* allocate_block(uint32_t capacity = 16);

    // Returns the slot offset on success, -1 if the block is full.
    int32_t try_append(BlockHeader* block, const Edge& e);

    size_t bytes_used()       const { return bump_.load(std::memory_order_relaxed); }
    size_t bytes_reserved()   const { return reserved_; }
    size_t blocks_allocated() const { return num_blocks_.load(std::memory_order_relaxed); }
    bool   uses_huge_pages()  const { return huge_pages_; }

private:
    char*               base_;
    size_t              reserved_;
    std::atomic<size_t> bump_{0};
    std::atomic<size_t> num_blocks_{0};
    bool                huge_pages_;
};

}  // namespace barry
