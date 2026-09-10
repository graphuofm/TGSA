#include "barry/engine.hpp"
#include <algorithm>
#include <queue>
#include <vector>

namespace barry {

// k-core decomposition (Matula--Beck-style peeling): repeatedly remove the
// minimum-degree vertex, recording its core number. Returns coreness[v].
//
// Implementation note: we materialize an undirected adjacency snapshot at the
// start (one neighbors_dst_into call per vertex) so the peeling loop runs on
// dense arrays — much cheaper than re-querying the vertex index per step.
std::vector<uint32_t> Barry::kcore() const {
    if (!vertex_index_) return {};
    const uint32_t V = vertex_index_->num_vertices();
    std::vector<std::vector<uint32_t>> adj(V);
    std::vector<uint32_t> deg(V, 0);

    {
        std::vector<uint32_t> nbr;
        nbr.reserve(64);
        for (uint32_t u = 0; u < V; ++u) {
            nbr.clear();
            vertex_index_->neighbors_dst_into(u, nbr);
            // Dedupe + treat undirected by mirroring.
            std::sort(nbr.begin(), nbr.end());
            nbr.erase(std::unique(nbr.begin(), nbr.end()), nbr.end());
            for (uint32_t v : nbr) {
                if (v < V && v != u) {
                    adj[u].push_back(v);
                    adj[v].push_back(u);
                }
            }
        }
        for (uint32_t v = 0; v < V; ++v) {
            std::sort(adj[v].begin(), adj[v].end());
            adj[v].erase(std::unique(adj[v].begin(), adj[v].end()), adj[v].end());
            deg[v] = static_cast<uint32_t>(adj[v].size());
        }
    }

    // Bin sort by current degree.
    uint32_t max_deg = 0;
    for (uint32_t v = 0; v < V; ++v) if (deg[v] > max_deg) max_deg = deg[v];

    std::vector<uint32_t> bin(max_deg + 2, 0);
    for (uint32_t v = 0; v < V; ++v) bin[deg[v]]++;
    uint32_t start = 0;
    for (uint32_t d = 0; d <= max_deg; ++d) {
        uint32_t cnt = bin[d];
        bin[d] = start;
        start += cnt;
    }
    std::vector<uint32_t> pos(V), order(V);
    for (uint32_t v = 0; v < V; ++v) {
        pos[v] = bin[deg[v]];
        order[pos[v]] = v;
        bin[deg[v]]++;
    }
    // Restore bin[d] = first index of degree-d vertices.
    for (uint32_t d = max_deg; d >= 1; --d) bin[d] = bin[d-1];
    bin[0] = 0;

    // Peel.
    std::vector<uint32_t> core(V, 0);
    for (uint32_t i = 0; i < V; ++i) {
        uint32_t v = order[i];
        core[v] = deg[v];
        for (uint32_t u : adj[v]) {
            if (deg[u] > deg[v]) {
                uint32_t du = deg[u], pu = pos[u];
                uint32_t pw = bin[du];
                uint32_t w = order[pw];
                if (u != w) {
                    pos[u] = pw; pos[w] = pu;
                    order[pu] = w; order[pw] = u;
                }
                bin[du]++;
                deg[u]--;
            }
        }
    }
    return core;
}

}  // namespace barry
