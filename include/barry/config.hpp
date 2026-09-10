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
    bool     use_huge_pages = false;
    uint32_t default_block_capacity = 16;
    uint32_t num_vertices = 0;                           // 0 = no vertex index built
    bool     build_reverse_index = false;                // also index in-edges (needed for backward tracing)
    size_t   vertex_arena_reserve_bytes = 8ULL << 30;    // 8 GB virtual reserve
    uint32_t target_avg_edges_per_window = 64;           // for shift auto-select
};

}  // namespace barry
