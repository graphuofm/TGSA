#include "barry/engine.hpp"
#include "test_common.hpp"

using namespace barry;

int main() {
    GraphConfig cfg;
    cfg.t_min = 0;
    cfg.t_max = 1ULL << 16;
    cfg.shift = 4;
    cfg.timeline_slots = 1ULL << 12;
    cfg.arena_reserve_bytes = 1ULL << 26;
    cfg.num_vertices = 8;
    cfg.vertex_arena_reserve_bytes = 1ULL << 22;
    cfg.default_block_capacity = 4;

    Barry b(cfg);
    b.insert({0, 1, 10, 1.0f});
    b.insert({1, 2, 20, 1.0f});
    b.insert({2, 3, 30, 1.0f});

    auto rng = b.query_range(0, 100);
    EXPECT_EQ(rng.size(), 3u);

    auto level = b.bfs(0);
    EXPECT_EQ(level[0], 0);
    EXPECT_EQ(level[1], 1);
    EXPECT_EQ(level[2], 2);
    EXPECT_EQ(level[3], 3);

    auto stats = b.stats();
    std::fprintf(stderr,
        "smoke: edges=%zu blocks=%zu arena_used=%zu rss=%.1f MB\n",
        stats.num_edges, stats.num_blocks, stats.bytes_arena_used,
        stats.bytes_rss / 1e6);
    EXPECT_EQ(stats.num_edges, 3u);
    return 0;
}
