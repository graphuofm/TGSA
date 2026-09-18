#pragma once
#include <cstdint>
#include <cstddef>

namespace barry {

constexpr size_t kCacheLineSize = 64;

struct GraphConfig {
    uint64_t t_min = 0;
    uint64_t t_max = 0;
    uint32_t shift = 0;                                  // 0 = auto-select on construction
    size_t   timeline_slots = 0;                         // 0 = auto from t range + headroom
    size_t   arena_reserve_bytes = 64ULL << 30;          // 64 GB virtual reserve
    bool     use_huge_pages = false;                     // explicit MAP_HUGETLB (needs reserved pages)
    // Ask the kernel to back the *dense* mappings (edge arena, vertex bucket
    // arena) with transparent huge pages. The timeline is never included: its
    // sparsity is the space bound, and 2 MB pages would make one touched window
    // dirty 2 MB instead of 4 KB.
    //
    // Off by default because it was measured and it loses. On 240M LANL edges
    // it cost 28% of ingest throughput (2.81 against 3.89 Meps) and 69% more
    // resident memory (15.9 against 9.4 GB). It was added while chasing a
    // superlinear slowdown at scale that turned out to be an arena leak; with
    // that fixed there is nothing here for huge pages to win back. The knob
    // stays so the measurement can be repeated, not because it helps.
    bool     transparent_huge_pages = false;
    uint32_t default_block_capacity = 16;
    uint32_t num_vertices = 0;                           // 0 = no vertex index built
    bool     build_reverse_index = false;                // also index in-edges (needed for backward tracing)
    size_t   vertex_arena_reserve_bytes = 8ULL << 30;    // 8 GB virtual reserve
    uint32_t target_avg_edges_per_window = 64;           // for shift auto-select

    // ------------------------------------------------------------------
    // Ablation switches. Defaults are the shipping configuration; each
    // exists so a design decision can be measured off as well as on,
    // rather than argued from the commit history.
    // ------------------------------------------------------------------
    // Claim a run of same-window arrivals with one counter bump (the fix
    // for time-ordered arrival being the worst case for parallel writers).
    // Off: every edge takes the single-edge insert path.
    bool     run_claim = true;
    // AVX2 boundary-window scan in query_range. Off by default because it was
    // measured and it loses: across four corpora and window widths from an
    // eighth of a slot to 64 slots, the gather-based vector path never beat
    // the scalar loop once, and cost up to 1.52x on narrow windows
    // (tc-cadets, results/e23_ablation/simd.csv). _mm256_i64gather_epi64 at a
    // 20-byte stride is slower than scalar loads on this microarchitecture,
    // and the vector path only ever touches a range's ~2 boundary slots, so
    // where it operates it loses and where it matters it idles. The knob
    // stays so the measurement can be repeated.
    bool     simd_scan = false;
    // Below this many refs, a sorted bucket is scanned rather than
    // binary-searched (cit-Patents, mean degree 2.7, exposed the seek
    // costing more than the read).
    uint32_t binary_search_min = 32;

    // ------------------------------------------------------------------
    // Dock modules. The core engine (timeline + arena + lock-free append +
    // window scans + retention) runs detached at full speed; each module
    // below docks a capability and pays a measured price on the write path
    // (results/e25_dock). num_vertices, build_reverse_index, and the WAL
    // (attached by the caller) are the other modules.
    // ------------------------------------------------------------------
    // Per-edge property payload, fixed size in bytes. 0 = detached. When
    // set, a parallel demand-paged sidecar holds properties addressed by
    // the same arithmetic that places the edge, and the props overloads of
    // insert/insert_batch store them under the publication protocol.
    uint32_t edge_property_bytes = 0;
};

}  // namespace barry
