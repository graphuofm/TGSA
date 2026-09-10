#pragma once
#include "edge.hpp"
#include "arena.hpp"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace barry {

// EdgeRef points back at an Edge stored in an Arena Block, with a cached
// peer-vertex id so pure-topology iteration (BFS / PR / TC) does not need
// to dereference `block` to read the full Edge.
//
// In the forward (out-edge) index `dst` holds the edge's destination; in the
// reverse (in-edge) index it holds the edge's source. Both are "the other
// endpoint", which is all topology-only traversal ever needs.
//
// Layout: 4 + 4 + 8 = 16 bytes (the previous {block,offset} layout was
// already padded to 16 bytes, so adding `dst` is free).
// Caching the timestamp here was measured and rejected: EdgeRef would grow to 24
// bytes (+50% on a vertex index that is already about half of RSS) in exchange for
// ~15% on traversal. The cost that actually dominates is not the Arena pointer
// chase but the bucket being in ARRIVAL order, so expansion is O(degree) where a
// time-sorted adjacency binary searches. That is structural, not a layout tweak.
struct EdgeRef {
    uint32_t     dst;       // cached peer id (destination, or source in the reverse index)
    uint32_t     offset;    // edge slot inside *block
    BlockHeader* block;     // owning block (needed for resolving the full Edge)
};
static_assert(sizeof(EdgeRef) == 16, "EdgeRef expected 16 bytes");

// A (peer, time) pair: everything a time-respecting expansion needs.
struct TemporalRef {
    uint32_t peer;
    uint64_t ts;
};

// `flags` carries kBucketSorted: set only by a quiescent finalize() pass that has
// verified the refs are in non-decreasing timestamp order, and cleared by any
// subsequent append. Audit streams arrive in time order (LANL: 0 out-of-order
// events in 1.05B), so in practice buckets are sorted and expansion can binary
// search instead of scanning the whole adjacency -- the difference between
// O(log d) pointer chases and O(d) at an average degree of ~60 000.
enum : uint32_t { kBucketSorted = 1u };

struct VertexBucket {
    std::atomic<uint32_t> count;
    uint32_t              capacity;
    VertexBucket*         next;
    std::atomic<uint32_t> flags;
    // EdgeRef[capacity] follows immediately.
};
static_assert(sizeof(VertexBucket) % alignof(EdgeRef) == 0,
              "EdgeRef alignment after VertexBucket header");

inline EdgeRef* refs_of(VertexBucket* b) {
    return reinterpret_cast<EdgeRef*>(b + 1);
}
inline const EdgeRef* refs_of(const VertexBucket* b) {
    return reinterpret_cast<const EdgeRef*>(b + 1);
}

// Out-edge index: vertex id -> list of EdgeRefs.
// Buckets are bump-allocated from a private mmap'd arena.
class VertexIndex {
public:
    VertexIndex(uint32_t num_vertices,
                size_t   arena_reserve_bytes = (8ULL << 30),
                bool     build_reverse = false);
    ~VertexIndex();

    VertexIndex(const VertexIndex&) = delete;
    VertexIndex& operator=(const VertexIndex&) = delete;

    // Append (dst, block, offset) under src -- and, when the reverse index is
    // enabled, (src, block, offset) under dst. Thread-safe.
    void register_edge(uint32_t src, uint32_t dst,
                       BlockHeader* block, uint32_t offset);

    // ----- Pure-topology iteration (no Edge resolve, alloc-free w/ buffer) -----
    // Append every out-neighbor's dst into `out` (does not clear it). Reads
    // only the cached dst, so no pointer-chase into the Arena.
    void neighbors_dst_into(uint32_t src, std::vector<uint32_t>& out) const;
    // Sort + dedupe variant for triangle counting.
    void sorted_neighbors_unique_into(uint32_t src,
                                      std::vector<uint32_t>& out) const;

    // ----- Full Edge resolve (needed when timestamps matter) -----
    std::vector<Edge> neighbors_of(uint32_t src) const;
    std::vector<Edge> neighbors_temporal(uint32_t src,
                                         uint64_t t_lo,
                                         uint64_t t_hi) const;

    // Allocation-free fast path for traversal: appends (peer, ts) for every
    // out-edge in [t_lo, t_hi] without touching the Arena. `out` is not cleared,
    // so a caller can reuse one scratch buffer across the whole traversal.
    void neighbors_temporal_into(uint32_t src, uint64_t t_lo, uint64_t t_hi,
                                 std::vector<TemporalRef>& out) const;

    // Sorted, deduplicated destination IDs (used by triangle counting).
    std::vector<uint32_t> sorted_neighbors_unique(uint32_t src) const;

    // ----- Reverse (in-edge) traversal: backward causality tracing -----
    // Only valid when the index was constructed with build_reverse = true.
    bool has_reverse() const noexcept { return heads_in_ != nullptr; }

    // Full Edge resolve of v's in-edges whose timestamp lies in [t_lo, t_hi].
    std::vector<Edge> in_neighbors_temporal(uint32_t dst,
                                            uint64_t t_lo,
                                            uint64_t t_hi) const;

    // Reverse-direction counterpart of neighbors_temporal_into.
    void in_neighbors_temporal_into(uint32_t dst, uint64_t t_lo, uint64_t t_hi,
                                    std::vector<TemporalRef>& out) const;

    // Single-threaded pass that marks every already-time-ordered bucket sorted,
    // enabling binary-search expansion. Safe to call repeatedly; must not run
    // concurrently with inserts. Returns the number of buckets marked.
    size_t finalize();

    uint32_t in_degree(uint32_t dst) const;

    uint32_t out_degree(uint32_t src) const;
    uint32_t num_vertices() const noexcept { return num_vertices_; }
    size_t   bytes_used()  const noexcept { return bucket_bump_.load(std::memory_order_relaxed); }

private:
    VertexBucket* allocate_bucket(uint32_t capacity);

    // Shared append path for both directions; `heads` selects the index.
    void append_ref(std::atomic<VertexBucket*>* heads, uint32_t key,
                    uint32_t peer, BlockHeader* block, uint32_t offset);

    VertexBucket* head_of(uint32_t src) const noexcept {
        return heads_[src].load(std::memory_order_acquire);
    }
    VertexBucket* head_in_of(uint32_t dst) const noexcept {
        return heads_in_[dst].load(std::memory_order_acquire);
    }

    uint32_t                   num_vertices_;
    std::atomic<VertexBucket*>* heads_;        // out-edges, length = num_vertices_
    std::atomic<VertexBucket*>* heads_in_;     // in-edges, length = num_vertices_ (or null)
    char*                      bucket_base_;
    size_t                     bucket_reserved_;
    std::atomic<size_t>        bucket_bump_{0};
};

}  // namespace barry
