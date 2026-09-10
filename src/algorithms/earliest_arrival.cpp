#include "barry/engine.hpp"
#include <climits>
#include <queue>
#include <utility>

namespace barry {

// Earliest Arrival (Wu et al., VLDB'14): time-respecting BFS variant via
// priority queue ordered by arrival time.
std::vector<uint64_t> Barry::earliest_arrival(uint32_t source,
                                              uint64_t t_start,
                                              uint64_t t_end) const {
    if (!vertex_index_) return {};
    const uint32_t V = vertex_index_->num_vertices();
    std::vector<uint64_t> ea(V, ULLONG_MAX);
    if (source >= V) return ea;

    ea[source] = t_start;

    using QItem = std::pair<uint64_t, uint32_t>;  // (arrival_time, vertex)
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> pq;
    pq.push({t_start, source});

    while (!pq.empty()) {
        auto [t_u, u] = pq.top();
        pq.pop();
        if (t_u > ea[u]) continue;

        // Time-respecting: only edges leaving u at time >= t_u count.
        auto out = vertex_index_->neighbors_temporal(u, t_u, t_end);
        for (const Edge& e : out) {
            if (e.dst >= V) continue;
            if (e.timestamp < ea[e.dst]) {
                ea[e.dst] = e.timestamp;
                pq.push({e.timestamp, e.dst});
            }
        }
    }
    return ea;
}

}  // namespace barry
