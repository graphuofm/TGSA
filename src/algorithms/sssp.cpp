#include "barry/engine.hpp"
#include <limits>
#include <queue>
#include <utility>

namespace barry {

// Single-source shortest path on weighted edges (Dijkstra with binary heap).
// Edge weights come from Edge.weight. NaN-tombstoned edges are skipped.
std::vector<float> Barry::sssp(uint32_t source) const {
    if (!vertex_index_) return {};
    const uint32_t V = vertex_index_->num_vertices();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    std::vector<float> dist(V, kInf);
    if (source >= V) return dist;
    dist[source] = 0.0f;

    using QItem = std::pair<float, uint32_t>;
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> pq;
    pq.push({0.0f, source});

    while (!pq.empty()) {
        auto [du, u] = pq.top(); pq.pop();
        if (du > dist[u]) continue;
        // Resolve full Edge for weight; this is the slow path. For uniform-
        // weight graphs SSSP degenerates to BFS.
        auto neighbors = vertex_index_->neighbors_of(u);
        for (const Edge& e : neighbors) {
            if (e.dst >= V) continue;
            float w = e.weight;
            if (!(w > 0.0f) || w != w) continue;   // skip 0/negative/NaN
            float nd = du + w;
            if (nd < dist[e.dst]) {
                dist[e.dst] = nd;
                pq.push({nd, e.dst});
            }
        }
    }
    return dist;
}

}  // namespace barry
