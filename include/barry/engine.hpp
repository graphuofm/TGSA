#pragma once
#include "arena.hpp"
#include "config.hpp"
#include "edge.hpp"
#include "time_mapper.hpp"
#include "timeline.hpp"
#include "vertex_index.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace barry {

class Barry {
public:
    explicit Barry(const GraphConfig& cfg);
    ~Barry();

    Barry(const Barry&) = delete;
    Barry& operator=(const Barry&) = delete;

    // Ingestion ---------------------------------------------------------
    void insert(const Edge& e);
    void insert_batch(const Edge* edges, size_t n, int num_threads = 0);
    void remove(uint32_t src, uint32_t dst, uint64_t ts);   // soft tombstone via weight=NaN

    // Temporal queries --------------------------------------------------
    std::vector<Edge> query_at(uint64_t timestamp) const;
    std::vector<Edge> query_range(uint64_t t_lo, uint64_t t_hi) const;
    double            window_aggregate_sum(uint64_t t_lo, uint64_t t_hi) const;

    // Standard graph algorithms ----------------------------------------
    std::vector<int32_t> bfs(uint32_t source) const;
    std::vector<float>   pagerank(int max_iter = 20, float damping = 0.85f) const;
    uint64_t             triangle_count() const;
    std::vector<float>   betweenness_centrality(int num_samples = 0) const;
    std::vector<float>   sssp(uint32_t source) const;            // weighted Dijkstra
    std::vector<int32_t> connected_components() const;            // returns component id per vertex
    std::vector<uint32_t> kcore() const;                          // returns coreness per vertex

    // Provenance / taint traversal --------------------------------------
    // A vertex together with the time it entered the result set.
    struct TaintNode {
        uint32_t vertex;
        uint64_t time;
    };

    // P2 -- forward taint: every vertex reachable from (seed, t_seed) along a
    // time-respecting path with timestamps in [t_seed, t_hi]. `time` is the
    // EARLIEST moment that vertex could have become tainted.
    // Output is sparse: only reached vertices, so cost is O(|result|), not O(V).
    // max_nodes = 0 means unbounded; otherwise stop after that many vertices.
    std::vector<TaintNode> forward_taint(uint32_t seed,
                                         uint64_t t_seed,
                                         uint64_t t_hi,
                                         size_t   max_nodes = 0) const;

    // P1 -- backward trace: every vertex that could causally have influenced
    // (alert, t_alert) along a time-respecting path with timestamps in
    // [t_lo, t_alert] (BackTracker semantics). `time` is the LATEST moment that
    // vertex could still have propagated influence to the alert.
    std::vector<TaintNode> backward_trace(uint32_t alert,
                                          uint64_t t_alert,
                                          uint64_t t_lo,
                                          size_t   max_nodes = 0) const;

    // Temporal-specific algorithms -------------------------------------
    std::vector<uint64_t> earliest_arrival(uint32_t source,
                                           uint64_t t_lo,
                                           uint64_t t_hi) const;
    uint64_t              temporal_triangle_count(uint64_t t_lo,
                                                  uint64_t t_hi,
                                                  uint64_t delta) const;

    // Compaction ---------------------------------------------------------
    // Sort each vertex's adjacency by timestamp so traversal can binary search.
    // Optional and explicit: queries are correct without it, and it is kept off
    // the insert path so appends stay O(batch). Returns buckets processed.
    size_t finalize();

    // Retention / lifecycle ---------------------------------------------
    // Drop everything older than `t`. Retention is itself a monotone time
    // predicate, so under an address mapping it reduces to releasing a prefix of
    // the timeline -- no scan of the edge set, no per-partition eviction pass.
    //
    // Physical pages are returned to the OS with MADV_DONTNEED. Anonymous pages
    // read back as zero afterwards, so a stale EdgeRef resolves to an Edge with
    // timestamp 0, which fails every `>= t_lo` predicate for t_lo > 0 and is
    // therefore invisible to temporal queries. Topology-only iteration still sees
    // the edge through the destination cached in the vertex index; a vertex-index
    // compaction pass would be needed to remove it, and is not implemented.
    struct EvictStats {
        size_t slots_cleared    = 0;   // timeline slots reset to null
        size_t blocks_released  = 0;   // arena blocks detached
        size_t bytes_spanned    = 0;   // total block bytes in the evicted prefix
        size_t bytes_released   = 0;   // bytes actually returned (page-aligned)
        size_t rss_before       = 0;
        size_t rss_after        = 0;
        double seconds          = 0.0;
    };
    EvictStats evict_before(uint64_t t);

    uint64_t min_valid_time() const noexcept { return min_valid_time_; }

    // Introspection ----------------------------------------------------
    struct Stats {
        size_t num_edges;
        size_t num_blocks;
        size_t bytes_arena_used;
        size_t bytes_arena_reserved;
        size_t bytes_timeline_virtual;
        size_t bytes_vertex_index;
        size_t bytes_rss;
    };
    Stats stats() const;

    const TimeMapper&  mapper()       const { return *mapper_; }
    const Timeline&    timeline()     const { return *timeline_; }
    const Arena&       arena()        const { return *arena_; }
    const VertexIndex* vertex_index() const { return vertex_index_.get(); }
    const GraphConfig& config()       const { return cfg_; }

private:
    BlockHeader* get_or_create_block(uint64_t idx);

    GraphConfig                  cfg_;
    std::unique_ptr<TimeMapper>  mapper_;
    std::unique_ptr<Timeline>    timeline_;
    std::unique_ptr<Arena>       arena_;
    std::unique_ptr<VertexIndex> vertex_index_;
    std::atomic<size_t>          num_inserts_{0};
    uint64_t                     min_valid_time_{0};
};

}  // namespace barry
