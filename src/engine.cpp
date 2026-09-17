#include "barry/engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#ifdef BARRY_HAVE_OPENMP
#include <omp.h>
#endif

namespace barry {

// Runtime SIMD switch, set at engine construction. A translation-unit global
// rather than a parameter because it gates an ablation, not per-call
// semantics; the last-constructed engine's setting wins, which is fine for
// benchmarks that build one engine per configuration.
namespace { bool g_simd_scan = true; }

Barry::Barry(const GraphConfig& cfg) : cfg_(cfg) {
    g_simd_scan = cfg_.simd_scan;
    if (cfg_.t_max < cfg_.t_min) {
        std::fprintf(stderr, "[Barry] invalid config: t_max < t_min\n");
        std::abort();
    }

    uint32_t shift = cfg_.shift;
    if (shift == 0 && cfg_.t_max > cfg_.t_min) {
        // No edge count yet — pick shift assuming ~1M edges by default.
        // Caller can override by setting cfg.shift explicitly.
        shift = TimeMapper::auto_select_shift_from_stats(
            1ULL << 20, cfg_.t_min, cfg_.t_max,
            cfg_.target_avg_edges_per_window);
    }

    size_t slots = cfg_.timeline_slots;
    if (slots == 0) {
        if (cfg_.t_max > cfg_.t_min) {
            slots = static_cast<size_t>(((cfg_.t_max - cfg_.t_min) >> shift) + 1024);
        } else {
            slots = 1ULL << 24;  // 16M default
        }
    }

    mapper_   = std::make_unique<TimeMapper>(cfg_.t_min, shift);
    timeline_ = std::make_unique<Timeline>(slots, cfg_.use_huge_pages);
    arena_    = std::make_unique<Arena>(cfg_.arena_reserve_bytes, cfg_.use_huge_pages,
                                        cfg_.transparent_huge_pages);
    if (cfg_.num_vertices > 0) {
        vertex_index_ = std::make_unique<VertexIndex>(
            cfg_.num_vertices, cfg_.vertex_arena_reserve_bytes,
            cfg_.build_reverse_index, cfg_.transparent_huge_pages,
            cfg_.binary_search_min);
    }
}

Barry::~Barry() = default;

BlockHeader* Barry::get_or_create_block(uint64_t idx) {
    BlockHeader* block = timeline_->load(idx);
    if (block) return block;

    BlockHeader* nb = arena_->allocate_block(cfg_.default_block_capacity);
    if (!nb) return nullptr;
    if (timeline_->cas(idx, nullptr, nb)) return nb;
    // Someone else won the CAS. Hand the block back rather than abandoning it.
    arena_->try_return_block(nb, cfg_.default_block_capacity);
    return timeline_->load(idx);
}

void Barry::insert(const Edge& e) {
    Edge ec = e;
    // ts == 0 is reserved as the unpublished-slot sentinel for concurrent
    // readers. A provenance engine must not silently drop a real event, so an
    // epoch-zero timestamp is clamped to 1 -- one time unit of error at the
    // epoch boundary instead of a lost record. (Audit across all 13 corpora:
    // exactly 2 such edges exist.)
    if (ec.timestamp == 0) ec.timestamp = 1;
    uint64_t idx = mapper_->map(ec.timestamp);
    BlockHeader* block = get_or_create_block(idx);
    if (!block) { edges_dropped_.fetch_add(1, std::memory_order_relaxed); return; }

    int32_t off;
    while (true) {
        off = arena_->try_append(block, ec);
        if (off >= 0) break;
        // Block is full; extend the chain. Re-read the link first: under N
        // writers on a hot window the usual case is that someone has already
        // extended it, and allocating before checking is what used to burn the
        // arena. Capacity doubles so the chain stays O(log) hops deep.
        BlockHeader* next = __atomic_load_n(&block->next, __ATOMIC_ACQUIRE);
        if (next == nullptr) {
            const uint32_t cap = block->capacity * 2;
            BlockHeader* nb = arena_->allocate_block(cap);
            if (!nb) { edges_dropped_.fetch_add(1, std::memory_order_relaxed); return; }
            BlockHeader* expected = nullptr;
            if (__atomic_compare_exchange_n(
                    &block->next, &expected, nb,
                    /*weak=*/false,
                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                next = nb;
            } else {
                arena_->try_return_block(nb, cap);
                next = expected;
            }
        }
        block = next;
    }

    if (vertex_index_) {
        vertex_index_->register_edge(ec.src, ec.dst, ec.timestamp, block,
                                     static_cast<uint32_t>(off));
    }
    num_inserts_.fetch_add(1, std::memory_order_relaxed);
}

// Insert a contiguous span, claiming slots a run at a time.
//
// Consecutive arrivals in an event stream nearly always share a window, so the
// whole run can be placed with one counter bump instead of one per edge. That
// matters because a time-ordered stream is precisely the case where every
// writer thread is appending into the same window at the same instant, making
// one block counter the engine's serialisation point.
void Barry::insert_span(const Edge* edges, size_t lo, size_t hi) {
    size_t i = lo;
    while (i < hi) {
        const uint64_t ts0 = edges[i].timestamp ? edges[i].timestamp : uint64_t{1};
        const uint64_t idx = mapper_->map(ts0);

        // Extend the run while the window is unchanged.
        size_t j = i + 1;
        while (j < hi) {
            const uint64_t ts = edges[j].timestamp ? edges[j].timestamp : uint64_t{1};
            if (mapper_->map(ts) != idx) break;
            ++j;
        }

        size_t placed = 0;
        const size_t run = j - i;
        BlockHeader* block = get_or_create_block(idx);
        if (!block) {
            edges_dropped_.fetch_add(run, std::memory_order_relaxed);
            i = j;
            continue;
        }

        while (placed < run) {
            const uint32_t want = static_cast<uint32_t>(
                std::min<size_t>(run - placed, 0xFFFFu));
            int32_t first = 0;
            const uint32_t took = arena_->try_append_run(block, edges + i + placed,
                                                         want, &first);
            if (took > 0) {
                if (vertex_index_) {
                    for (uint32_t k = 0; k < took; ++k) {
                        const Edge& e = edges[i + placed + k];
                        vertex_index_->register_edge(
                            e.src, e.dst,
                            e.timestamp ? e.timestamp : uint64_t{1},
                            block, static_cast<uint32_t>(first) + k);
                    }
                }
                placed += took;
                num_inserts_.fetch_add(took, std::memory_order_relaxed);
                continue;
            }
            // Block full: extend the chain, re-reading the link first so the
            // common case where another writer already extended it allocates
            // nothing.
            BlockHeader* next = __atomic_load_n(&block->next, __ATOMIC_ACQUIRE);
            if (next == nullptr) {
                const uint32_t cap = block->capacity * 2;
                BlockHeader* nb = arena_->allocate_block(cap);
                if (!nb) {
                    edges_dropped_.fetch_add(run - placed, std::memory_order_relaxed);
                    break;
                }
                BlockHeader* expected = nullptr;
                if (__atomic_compare_exchange_n(&block->next, &expected, nb,
                                                /*weak=*/false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    next = nb;
                } else {
                    arena_->try_return_block(nb, cap);
                    next = expected;
                }
            }
            block = next;
        }
        i = j;
    }
}

void Barry::insert_batch(const Edge* edges, size_t n, int num_threads) {
    // Chunk explicitly rather than letting the parallel-for hand out single
    // iterations: a run can only be detected inside one thread's contiguous
    // span, and a static chunk of 1024 keeps that span long enough to matter.
    constexpr size_t kChunk = 1024;
    const size_t chunks = (n + kChunk - 1) / kChunk;
#ifdef BARRY_HAVE_OPENMP
    if (num_threads <= 0) num_threads = omp_get_max_threads();
    #pragma omp parallel for num_threads(num_threads) schedule(static)
    for (size_t c = 0; c < chunks; ++c) {
        const size_t lo = c * kChunk;
        const size_t hi = std::min(n, lo + kChunk);
        if (cfg_.run_claim) insert_span(edges, lo, hi);
        else for (size_t i = lo; i < hi; ++i) insert(edges[i]);
    }
#else
    (void)num_threads;
    for (size_t c = 0; c < chunks; ++c) {
        const size_t lo = c * kChunk;
        const size_t hi = std::min(n, lo + kChunk);
        if (cfg_.run_claim) insert_span(edges, lo, hi);
        else for (size_t i = lo; i < hi; ++i) insert(edges[i]);
    }
#endif
}

// Explicit, optional compaction: sorts each vertex's adjacency by time so
// traversal can binary search instead of scanning. Deliberately NOT called from
// insert_batch -- it walks the entire index, so doing it per batch would make
// incremental ingestion cost O(index) instead of O(batch) and forfeit the one
// advantage Barry has over a statically sorted structure. Callers that bulk-load
// and then query should call it once; a streaming deployment schedules it like a
// compaction. Queries remain correct without it, just linear per expansion.
size_t Barry::finalize() {
    // Mark arena blocks whose every slot is published: readers then take the
    // no-check bulk path. Quiescence is the caller's contract (as for the
    // bucket sort below), so a plain verify-then-set is sound.
    for (uint64_t idx = 0; idx < timeline_->num_slots(); ++idx) {
        for (BlockHeader* b = timeline_->load(idx); b; b = b->next) {
            uint32_t cnt = std::min(b->count.load(std::memory_order_acquire), b->capacity);
            const Edge* arr = edges_of(b);
            bool all = true;
            for (uint32_t i = 0; i < cnt; ++i)
                if (arr[i].timestamp == 0) { all = false; break; }
            if (all) b->flags.store(kBlockPublished, std::memory_order_release);
        }
    }
    return vertex_index_ ? vertex_index_->finalize() : 0;
}

void Barry::remove(uint32_t src, uint32_t dst, uint64_t ts) {
    uint64_t idx = mapper_->map(ts);
    BlockHeader* block = timeline_->load(idx);
    while (block) {
        uint32_t cnt = std::min(block->count.load(std::memory_order_acquire),
                                block->capacity);
        Edge* arr = edges_of(block);
        for (uint32_t i = 0; i < cnt; ++i) {
            if (arr[i].src == src && arr[i].dst == dst && arr[i].timestamp == ts) {
                arr[i].weight = std::nanf("");
                return;
            }
        }
        block = block->next;
    }
}

static bool is_alive(const Edge& e) {
    return !std::isnan(e.weight);
}

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace {

// Append every alive edge in [arr, arr+cnt) whose timestamp lies in [t_lo, t_hi].
// Edges inside a block are contiguous packed 20-byte structs, which is what makes a
// vectorized scan possible here -- the vertex index cannot do the same because its
// refs dereference into the arena. With AVX2 we gather 4 timestamps per step (byte
// offsets 8,28,48,68 relative to the cursor). Timestamps are < 2^63 (nanosecond
// epochs are ~1.5e18), so signed 64-bit compares are exact.
inline void scan_block_filtered(const BlockHeader* b, const Edge* arr, uint32_t cnt,
                                uint64_t t_lo, uint64_t t_hi,
                                std::vector<Edge>& out) {
    const bool published = b->flags.load(std::memory_order_acquire) & kBlockPublished;
    uint32_t i = 0;
#if defined(__AVX2__)
    // Concurrent-publish note: the gather reads timestamps with plain loads; a
    // zero (unpublished) slot fails the range test because t_lo >= 1. The fence
    // below upgrades the mask-dependent payload reads to acquire semantics.
#endif
#if defined(__AVX2__)
    if (g_simd_scan) {
    const __m256i vidx = _mm256_setr_epi64x(8, 28, 48, 68);
    const __m256i vlo  = _mm256_set1_epi64x(static_cast<long long>(t_lo) - 1);
    const __m256i vhi  = _mm256_set1_epi64x(static_cast<long long>(t_hi) + 1);
    for (; i + 4 <= cnt; i += 4) {
        const long long* base = reinterpret_cast<const long long*>(arr + i);
        __m256i ts = _mm256_i64gather_epi64(base, vidx, 1);
        __m256i ok = _mm256_and_si256(_mm256_cmpgt_epi64(ts, vlo),
                                      _mm256_cmpgt_epi64(vhi, ts));
        int mask = _mm256_movemask_pd(_mm256_castsi256_pd(ok));
        if (mask) __atomic_thread_fence(__ATOMIC_ACQUIRE);
        while (mask) {
            int j = __builtin_ctz(mask);
            mask &= mask - 1;
            const Edge& e = arr[i + j];
            if (is_alive(e)) out.push_back(e);
        }
    }
    }
#endif
    for (; i < cnt; ++i) {
        uint64_t ts = published ? arr[i].timestamp
                                : __atomic_load_n(&arr[i].timestamp, __ATOMIC_ACQUIRE);
        if (ts >= t_lo && ts <= t_hi && ts != 0 && is_alive(arr[i]))
            out.push_back(arr[i]);
    }
}

// Interior windows of a range query lie entirely inside [t_lo, t_hi]: every alive
// edge qualifies and no timestamp is ever compared.
inline void copy_block_alive(const BlockHeader* b, const Edge* arr, uint32_t cnt,
                             std::vector<Edge>& out) {
    // Count was loaded before this flag read; the append protocol (clear flag,
    // then bump) makes an observed flag prove the counted prefix is verified.
    if (b->flags.load(std::memory_order_acquire) & kBlockPublished) {
        for (uint32_t i = 0; i < cnt; ++i)
            if (is_alive(arr[i])) out.push_back(arr[i]);
        return;
    }
    for (uint32_t i = 0; i < cnt; ++i) {
        if (__atomic_load_n(&arr[i].timestamp, __ATOMIC_ACQUIRE) == 0) continue;  // unpublished
        if (is_alive(arr[i])) out.push_back(arr[i]);
    }
}

}  // namespace

std::vector<Edge> Barry::query_at(uint64_t timestamp) const {
    std::vector<Edge> out;
    uint64_t idx = mapper_->map(timestamp);
    if (idx >= timeline_->num_slots()) return out;

    BlockHeader* block = timeline_->load(idx);
    while (block) {
        uint32_t cnt = std::min(block->count.load(std::memory_order_acquire),
                                block->capacity);
        const Edge* arr = edges_of(block);
        for (uint32_t i = 0; i < cnt; ++i) {
            uint64_t ts = __atomic_load_n(&arr[i].timestamp, __ATOMIC_ACQUIRE);
            if (ts == timestamp && ts != 0 && is_alive(arr[i])) out.push_back(arr[i]);
        }
        block = block->next;
    }
    return out;
}

std::vector<Edge> Barry::query_range(uint64_t t_lo, uint64_t t_hi) const {
    std::vector<Edge> out;
    if (t_hi < t_lo) return out;

    uint64_t idx_lo = mapper_->map(t_lo);
    uint64_t idx_hi = mapper_->map(t_hi);
    size_t   max_idx = timeline_->num_slots();
    if (idx_lo >= max_idx) return out;
    if (idx_hi >= max_idx) idx_hi = max_idx - 1;

    // Pre-size the result from block headers alone (no edge data touched): the sum
    // of counts is exact for interior windows and an upper bound at the boundaries,
    // so the fill loop below never reallocates.
    {
        size_t cap = 0;
        for (uint64_t idx = idx_lo; idx <= idx_hi; ++idx)
            for (BlockHeader* b = timeline_->load(idx); b; b = b->next)
                cap += std::min(b->count.load(std::memory_order_acquire), b->capacity);
        out.reserve(cap);
    }

    for (uint64_t idx = idx_lo; idx <= idx_hi; ++idx) {
        // Only the two boundary windows can contain out-of-range edges; interior
        // windows map entirely inside [t_lo, t_hi] by construction of the shift.
        const bool boundary = (idx == idx_lo) || (idx == idx_hi);
        BlockHeader* block = timeline_->load(idx);
        while (block) {
            uint32_t cnt = std::min(block->count.load(std::memory_order_acquire),
                                    block->capacity);
            const Edge* arr = edges_of(block);
            if (boundary) scan_block_filtered(block, arr, cnt, t_lo, t_hi, out);
            else          copy_block_alive(block, arr, cnt, out);
            block = block->next;
        }
    }
    return out;
}

double Barry::window_aggregate_sum(uint64_t t_lo, uint64_t t_hi) const {
    double sum = 0.0;
    if (t_hi < t_lo) return sum;

    uint64_t idx_lo = mapper_->map(t_lo);
    uint64_t idx_hi = mapper_->map(t_hi);
    size_t   max_idx = timeline_->num_slots();
    if (idx_lo >= max_idx) return sum;
    if (idx_hi >= max_idx) idx_hi = max_idx - 1;

    for (uint64_t idx = idx_lo; idx <= idx_hi; ++idx) {
        BlockHeader* block = timeline_->load(idx);
        while (block) {
            uint32_t cnt = std::min(block->count.load(std::memory_order_acquire),
                                    block->capacity);
            const Edge* arr = edges_of(block);
            for (uint32_t i = 0; i < cnt; ++i) {
                if (__atomic_load_n(&arr[i].timestamp, __ATOMIC_ACQUIRE) == 0) continue;
                if (arr[i].timestamp >= t_lo && arr[i].timestamp <= t_hi
                    && is_alive(arr[i])) {
                    sum += arr[i].weight;
                }
            }
            block = block->next;
        }
    }
    return sum;
}

Barry::Stats Barry::stats() const {
    Stats s{};
    s.num_edges               = num_inserts_.load(std::memory_order_relaxed);
    s.edges_dropped           = edges_dropped_.load(std::memory_order_relaxed);
    s.blocks_returned         = arena_->blocks_returned();
    s.num_blocks              = arena_->blocks_allocated();
    s.bytes_arena_used        = arena_->bytes_used();
    s.bytes_arena_reserved    = arena_->bytes_reserved();
    s.bytes_timeline_virtual  = timeline_->bytes_virtual();
    s.bytes_vertex_index      = vertex_index_ ? vertex_index_->bytes_used() : 0;
    s.bytes_rss               = Timeline::process_rss_bytes();
    return s;
}

}  // namespace barry
