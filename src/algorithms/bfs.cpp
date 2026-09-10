#include "barry/engine.hpp"
#include <queue>

namespace barry {

std::vector<int32_t> Barry::bfs(uint32_t source) const {
    if (!vertex_index_) return {};
    const uint32_t V = vertex_index_->num_vertices();
    std::vector<int32_t> level(V, -1);
    if (source >= V) return level;

    level[source] = 0;
    std::queue<uint32_t> q;
    q.push(source);

    // Reusable scratch buffer — one heap alloc for the whole traversal instead
    // of one per visited vertex.
    std::vector<uint32_t> nbr;
    nbr.reserve(64);

    while (!q.empty()) {
        uint32_t u = q.front();
        q.pop();
        nbr.clear();
        vertex_index_->neighbors_dst_into(u, nbr);
        for (uint32_t v : nbr) {
            if (v < V && level[v] == -1) {
                level[v] = level[u] + 1;
                q.push(v);
            }
        }
    }
    return level;
}

}  // namespace barry
