#include "barry/vertex_index.hpp"
#include <sys/mman.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace barry {

static constexpr uint32_t kInitialBucketCap = 8;

static void* try_mmap(size_t bytes) {
    return mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

VertexIndex::VertexIndex(uint32_t num_vertices, size_t arena_reserve_bytes,
                         bool build_reverse)
    : num_vertices_(num_vertices),
      heads_(nullptr),
      heads_in_(nullptr),
      bucket_base_(nullptr),
      bucket_reserved_(arena_reserve_bytes) {

    if (num_vertices == 0) {
        std::fprintf(stderr, "[VertexIndex] num_vertices=0, disabled\n");
        return;
    }

    size_t heads_bytes = static_cast<size_t>(num_vertices) * sizeof(std::atomic<VertexBucket*>);
    void* hp = try_mmap(heads_bytes);
    if (hp == MAP_FAILED) {
        std::perror("[VertexIndex] mmap heads"); std::abort();
    }
    heads_ = reinterpret_cast<std::atomic<VertexBucket*>*>(hp);
    // mmap zero-initialises, so all heads start as nullptr.

    if (build_reverse) {
        void* hip = try_mmap(heads_bytes);
        if (hip == MAP_FAILED) {
            std::perror("[VertexIndex] mmap reverse heads"); std::abort();
        }
        heads_in_ = reinterpret_cast<std::atomic<VertexBucket*>*>(hip);
    }

    void* bp = try_mmap(bucket_reserved_);
    if (bp == MAP_FAILED) {
        std::perror("[VertexIndex] mmap bucket arena"); std::abort();
    }
    bucket_base_ = static_cast<char*>(bp);

    std::fprintf(stderr,
        "[VertexIndex] %u vertices, heads=%.1f MB%s, bucket arena=%.1f GB virtual\n",
        num_vertices, heads_bytes / 1e6,
        heads_in_ ? " x2 (reverse index on)" : "",
        bucket_reserved_ / 1e9);
}

VertexIndex::~VertexIndex() {
    size_t heads_bytes = static_cast<size_t>(num_vertices_) * sizeof(std::atomic<VertexBucket*>);
    if (heads_) {
        munmap(heads_, heads_bytes);
    }
    if (heads_in_) {
        munmap(heads_in_, heads_bytes);
    }
    if (bucket_base_) {
        munmap(bucket_base_, bucket_reserved_);
    }
}

VertexBucket* VertexIndex::allocate_bucket(uint32_t capacity) {
    size_t total = sizeof(VertexBucket) + static_cast<size_t>(capacity) * sizeof(EdgeRef);
    // 8-byte align next allocation (EdgeRef is 8-byte aligned already, header too).
    total = (total + 7) & ~size_t{7};

    size_t off = bucket_bump_.fetch_add(total, std::memory_order_relaxed);
    if (off + total > bucket_reserved_) {
        std::fprintf(stderr, "[VertexIndex] bucket arena OOM\n");
        return nullptr;
    }
    auto* b = reinterpret_cast<VertexBucket*>(bucket_base_ + off);
    b->count.store(0, std::memory_order_relaxed);
    b->capacity = capacity;
    b->next = nullptr;
    b->flags.store(0, std::memory_order_relaxed);   // unsorted until finalize() says otherwise
    return b;
}

void VertexIndex::append_ref(std::atomic<VertexBucket*>* heads, uint32_t key,
                             uint32_t peer, BlockHeader* block, uint32_t offset) {
    VertexBucket* head = heads[key].load(std::memory_order_acquire);
    if (head == nullptr) {
        VertexBucket* nb = allocate_bucket(kInitialBucketCap);
        if (!nb) return;
        VertexBucket* expected = nullptr;
        if (heads[key].compare_exchange_strong(
                expected, nb,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            head = nb;
        } else {
            head = expected;
            // nb is leaked into the bump arena -- acceptable; we never free buckets.
        }
    }

    VertexBucket* b = head;
    for (;;) {
        // Order matters for concurrent readers: clear the sorted flag BEFORE
        // bumping the count. A reader loads count (acquire) and then the flag;
        // if it still observes kBucketSorted, the acquire on count synchronizes
        // with every prior bump's release, and each bump was preceded by a
        // clear -- so a sorted flag proves the loaded count covers only the
        // verified prefix, and binary search over it is sound.
        b->flags.store(0, std::memory_order_release);
        uint32_t pos = b->count.fetch_add(1, std::memory_order_acq_rel);
        if (pos < b->capacity) {
            EdgeRef* r = &refs_of(b)[pos];
            r->dst = peer; r->offset = offset;
            // Publish protocol: block pointer last, release. Zero pages mean an
            // unpublished ref reads block == nullptr; every reader skips those.
            __atomic_store_n(&r->block, block, __ATOMIC_RELEASE);
            return;
        }
        // Bucket is full -- push a new one onto the front of the chain so future
        // appends find space immediately.
        VertexBucket* nb = allocate_bucket(b->capacity * 2);
        if (!nb) return;
        nb->next = b;
        VertexBucket* expected = b;
        if (heads[key].compare_exchange_strong(
                expected, nb,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            b = nb;
        } else {
            // Someone else extended the chain first -- retry against the new head.
            b = expected;
        }
    }
}

void VertexIndex::register_edge(uint32_t src, uint32_t dst,
                                BlockHeader* block, uint32_t offset) {
    if (heads_ == nullptr) return;
    if (src < num_vertices_) append_ref(heads_, src, dst, block, offset);
    if (heads_in_ && dst < num_vertices_) append_ref(heads_in_, dst, src, block, offset);
}

static Edge resolve(const EdgeRef& r) {
    // Reading edges_of(block)[offset] is safe even if more edges have been
    // appended since: the block's slot for this offset is written exactly once.
    return edges_of(r.block)[r.offset];
}

// Collects (peer, ts) for refs in [t_lo, t_hi]. When the bucket has been verified
// time-ordered, seeks with a binary search (O(log n) resolves) and stops at the
// first ref past t_hi; otherwise falls back to a full scan.
static void collect_range(const VertexBucket* b, uint64_t t_lo, uint64_t t_hi,
                          std::vector<TemporalRef>& out) {
    uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
    const EdgeRef* refs = refs_of(b);
    if (cnt == 0) return;

    if (b->flags.load(std::memory_order_acquire) & kBucketSorted) {
        uint32_t lo = 0, hi = cnt;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (resolve(refs[mid]).timestamp < t_lo) lo = mid + 1; else hi = mid;
        }
        for (uint32_t i = lo; i < cnt; ++i) {
            uint64_t ts = resolve(refs[i]).timestamp;
            if (ts > t_hi) break;
            out.push_back(TemporalRef{refs[i].dst, ts});
        }
        return;
    }
    for (uint32_t i = 0; i < cnt; ++i) {
        if (!__atomic_load_n(&refs[i].block, __ATOMIC_ACQUIRE)) continue;   // unpublished
        uint64_t ts = resolve(refs[i]).timestamp;
        if (ts >= t_lo && ts <= t_hi && ts != 0) out.push_back(TemporalRef{refs[i].dst, ts});
    }
}

void VertexIndex::neighbors_temporal_into(uint32_t src, uint64_t t_lo, uint64_t t_hi,
                                          std::vector<TemporalRef>& out) const {
    if (heads_ == nullptr || src >= num_vertices_) return;
    for (VertexBucket* b = head_of(src); b; b = b->next) collect_range(b, t_lo, t_hi, out);
}

size_t VertexIndex::finalize() {
    size_t marked = 0;
    if (heads_ == nullptr) return 0;

    // Parallel ingestion races on the bucket's append counter, so even a
    // perfectly time-ordered input lands out of order inside a bucket (measured:
    // 8 threads leave only ~56% of buckets ordered). Rather than give up the
    // parallel insert, sort each bucket once here. Appends stay O(1); only the
    // query-ready state costs a pass, and it is per-bucket, never a global sort.
    //
    // Timestamps are resolved into a scratch buffer first so the comparator does
    // not chase a pointer per comparison.
    struct Keyed { uint64_t ts; EdgeRef ref; };
    std::vector<Keyed> scratch;

    auto process = [&](std::atomic<VertexBucket*>* heads) {
        if (!heads) return;
        for (uint32_t v = 0; v < num_vertices_; ++v) {
            for (VertexBucket* b = heads[v].load(std::memory_order_acquire); b; b = b->next) {
                uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
                if (cnt == 0) {
                    b->flags.store(kBucketSorted, std::memory_order_release);
                    ++marked;
                    continue;
                }
                EdgeRef* refs = refs_of(b);

                scratch.clear();
                scratch.reserve(cnt);
                bool ordered = true;
                uint64_t prev = 0;
                for (uint32_t i = 0; i < cnt; ++i) {
                    uint64_t ts = resolve(refs[i]).timestamp;
                    if (ts < prev) ordered = false;
                    prev = ts;
                    scratch.push_back(Keyed{ts, refs[i]});
                }
                if (!ordered) {
                    std::sort(scratch.begin(), scratch.end(),
                              [](const Keyed& a, const Keyed& b2) { return a.ts < b2.ts; });
                    for (uint32_t i = 0; i < cnt; ++i) refs[i] = scratch[i].ref;
                }
                b->flags.store(kBucketSorted, std::memory_order_release);
                ++marked;
            }
        }
    };
    process(heads_);
    process(heads_in_);
    return marked;
}

void VertexIndex::neighbors_weighted_into(uint32_t src,
                                          std::vector<std::pair<uint32_t,float>>& out) const {
    if (heads_ == nullptr || src >= num_vertices_) return;
    for (VertexBucket* b = head_of(src); b; b = b->next) {
        uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
        const EdgeRef* refs = refs_of(b);
        for (uint32_t i = 0; i < cnt; ++i)
            if (__atomic_load_n(&refs[i].block, __ATOMIC_ACQUIRE))
                out.emplace_back(refs[i].dst, resolve(refs[i]).weight);
    }
}

void VertexIndex::in_neighbors_temporal_into(uint32_t dst, uint64_t t_lo, uint64_t t_hi,
                                             std::vector<TemporalRef>& out) const {
    if (heads_in_ == nullptr || dst >= num_vertices_) return;
    for (VertexBucket* b = head_in_of(dst); b; b = b->next) collect_range(b, t_lo, t_hi, out);
}

void VertexIndex::neighbors_dst_into(uint32_t src,
                                     std::vector<uint32_t>& out) const {
    if (heads_ == nullptr || src >= num_vertices_) return;
    for (VertexBucket* b = head_of(src); b; b = b->next) {
        uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
        const EdgeRef* refs = refs_of(b);
        for (uint32_t i = 0; i < cnt; ++i)
            if (__atomic_load_n(&refs[i].block, __ATOMIC_ACQUIRE)) out.push_back(refs[i].dst);
    }
}

void VertexIndex::sorted_neighbors_unique_into(uint32_t src,
                                               std::vector<uint32_t>& out) const {
    size_t base = out.size();
    neighbors_dst_into(src, out);
    std::sort(out.begin() + base, out.end());
    out.erase(std::unique(out.begin() + base, out.end()), out.end());
}

std::vector<Edge> VertexIndex::neighbors_of(uint32_t src) const {
    std::vector<Edge> out;
    if (heads_ == nullptr || src >= num_vertices_) return out;

    for (VertexBucket* b = head_of(src); b; b = b->next) {
        uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
        const EdgeRef* refs = refs_of(b);
        for (uint32_t i = 0; i < cnt; ++i)
            if (__atomic_load_n(&refs[i].block, __ATOMIC_ACQUIRE)) out.push_back(resolve(refs[i]));
    }
    return out;
}

std::vector<Edge> VertexIndex::neighbors_temporal(uint32_t src,
                                                  uint64_t t_lo,
                                                  uint64_t t_hi) const {
    std::vector<Edge> out;
    if (heads_ == nullptr || src >= num_vertices_) return out;

    for (VertexBucket* b = head_of(src); b; b = b->next) {
        uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
        const EdgeRef* refs = refs_of(b);
        for (uint32_t i = 0; i < cnt; ++i) {
            if (!__atomic_load_n(&refs[i].block, __ATOMIC_ACQUIRE)) continue;
            Edge e = resolve(refs[i]);
            if (e.timestamp >= t_lo && e.timestamp <= t_hi && e.timestamp != 0) out.push_back(e);
        }
    }
    return out;
}

std::vector<uint32_t> VertexIndex::sorted_neighbors_unique(uint32_t src) const {
    std::vector<uint32_t> ids;
    if (heads_ == nullptr || src >= num_vertices_) return ids;

    for (VertexBucket* b = head_of(src); b; b = b->next) {
        uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
        const EdgeRef* refs = refs_of(b);
        for (uint32_t i = 0; i < cnt; ++i)
            if (__atomic_load_n(&refs[i].block, __ATOMIC_ACQUIRE)) ids.push_back(resolve(refs[i]).dst);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::vector<Edge> VertexIndex::in_neighbors_temporal(uint32_t dst,
                                                     uint64_t t_lo,
                                                     uint64_t t_hi) const {
    std::vector<Edge> out;
    if (heads_in_ == nullptr || dst >= num_vertices_) return out;

    for (VertexBucket* b = head_in_of(dst); b; b = b->next) {
        uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
        const EdgeRef* refs = refs_of(b);
        for (uint32_t i = 0; i < cnt; ++i) {
            if (!__atomic_load_n(&refs[i].block, __ATOMIC_ACQUIRE)) continue;
            Edge e = resolve(refs[i]);
            if (e.timestamp >= t_lo && e.timestamp <= t_hi && e.timestamp != 0) out.push_back(e);
        }
    }
    return out;
}

uint32_t VertexIndex::in_degree(uint32_t dst) const {
    if (heads_in_ == nullptr || dst >= num_vertices_) return 0;
    uint32_t total = 0;
    for (VertexBucket* b = head_in_of(dst); b; b = b->next) {
        total += std::min(b->count.load(std::memory_order_acquire), b->capacity);
    }
    return total;
}

uint32_t VertexIndex::out_degree(uint32_t src) const {
    if (heads_ == nullptr || src >= num_vertices_) return 0;
    uint32_t total = 0;
    for (VertexBucket* b = head_of(src); b; b = b->next) {
        total += std::min(b->count.load(std::memory_order_acquire), b->capacity);
    }
    return total;
}

}  // namespace barry
