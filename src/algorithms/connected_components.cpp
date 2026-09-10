#include "barry/engine.hpp"
#include <queue>

namespace barry {

// Connected components via BFS. Treats the graph as undirected: when we visit
// vertex u via its out-edges, we also fold in successors of u, but we trust
// caller to provide a symmetric edge stream if undirected semantics are wanted.
// Component id = smallest source-vertex that reached the component.
std::vector<int32_t> Barry::connected_components() const {
    if (!vertex_index_) return {};
    const uint32_t V = vertex_index_->num_vertices();
    std::vector<int32_t> comp(V, -1);

    std::vector<uint32_t> nbr;
    nbr.reserve(64);

    for (uint32_t s = 0; s < V; ++s) {
        if (comp[s] != -1) continue;
        comp[s] = static_cast<int32_t>(s);
        std::queue<uint32_t> q;
        q.push(s);
        while (!q.empty()) {
            uint32_t u = q.front(); q.pop();
            nbr.clear();
            vertex_index_->neighbors_dst_into(u, nbr);
            for (uint32_t v : nbr) {
                if (v < V && comp[v] == -1) {
                    comp[v] = static_cast<int32_t>(s);
                    q.push(v);
                }
            }
        }
    }
    return comp;
}

}  // namespace barry
