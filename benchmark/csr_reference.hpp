// Minimal static CSR for graph algorithm baselines. Stores src->dst edges
// (no timestamps) and provides BFS / PageRank / Triangle Counting that
// match the semantics of the Barry implementations as closely as possible.

#pragma once

#include "barry/edge.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#ifdef BARRY_HAVE_OPENMP
#include <omp.h>
#endif

namespace barry { namespace bench {

class CsrGraph {
public:
    void build(const Edge* edges, size_t n, uint32_t V) {
        V_ = V;
        offsets_.assign(V + 1, 0);
        for (size_t i = 0; i < n; ++i) offsets_[edges[i].src + 1]++;
        for (uint32_t v = 0; v < V; ++v) offsets_[v + 1] += offsets_[v];
        nbrs_.assign(n, 0);
        std::vector<uint64_t> head = offsets_;
        for (size_t i = 0; i < n; ++i) nbrs_[head[edges[i].src]++] = edges[i].dst;
    }

    uint32_t num_vertices() const { return V_; }
    uint64_t out_offset(uint32_t v) const { return offsets_[v]; }
    uint64_t out_offset_end(uint32_t v) const { return offsets_[v + 1]; }
    const uint32_t* nbr_data() const { return nbrs_.data(); }
    uint32_t out_degree(uint32_t v) const { return offsets_[v+1] - offsets_[v]; }

    std::vector<int32_t> bfs(uint32_t source) const {
        std::vector<int32_t> level(V_, -1);
        if (source >= V_) return level;
        level[source] = 0;
        std::vector<uint32_t> frontier{source};
        std::vector<uint32_t> next;
        int32_t lvl = 0;
        while (!frontier.empty()) {
            ++lvl;
            next.clear();
            for (uint32_t u : frontier) {
                for (uint64_t i = offsets_[u]; i < offsets_[u+1]; ++i) {
                    uint32_t v = nbrs_[i];
                    if (level[v] == -1) { level[v] = lvl; next.push_back(v); }
                }
            }
            std::swap(frontier, next);
        }
        return level;
    }

    std::vector<float> pagerank(int max_iter = 20, float damping = 0.85f) const {
        std::vector<float> pr(V_, 1.0f / V_), next_pr(V_);
        const float teleport = (1.0f - damping) / V_;
        for (int it = 0; it < max_iter; ++it) {
            std::fill(next_pr.begin(), next_pr.end(), teleport);
#ifdef BARRY_HAVE_OPENMP
            #pragma omp parallel for schedule(dynamic, 1024)
#endif
            for (uint32_t u = 0; u < V_; ++u) {
                uint32_t deg = offsets_[u+1] - offsets_[u];
                if (!deg) continue;
                float share = damping * pr[u] / deg;
                for (uint64_t i = offsets_[u]; i < offsets_[u+1]; ++i) {
                    uint32_t v = nbrs_[i];
#ifdef BARRY_HAVE_OPENMP
                    #pragma omp atomic
#endif
                    next_pr[v] += share;
                }
            }
            std::swap(pr, next_pr);
        }
        return pr;
    }

    // SSSP with Dijkstra. Edges stored without weights in CSR — caller must
    // supply per-edge weights (vector indexed by nbr position).
    std::vector<float> sssp(uint32_t source, const std::vector<float>& weights) const {
        constexpr float kInf = std::numeric_limits<float>::infinity();
        std::vector<float> dist(V_, kInf);
        if (source >= V_) return dist;
        dist[source] = 0.0f;
        using QItem = std::pair<float, uint32_t>;
        std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> pq;
        pq.push({0.0f, source});
        while (!pq.empty()) {
            auto [du, u] = pq.top(); pq.pop();
            if (du > dist[u]) continue;
            for (uint64_t i = offsets_[u]; i < offsets_[u+1]; ++i) {
                uint32_t v = nbrs_[i];
                float w = (i < weights.size()) ? weights[i] : 1.0f;
                if (!(w > 0.0f) || w != w) continue;
                float nd = du + w;
                if (nd < dist[v]) { dist[v] = nd; pq.push({nd, v}); }
            }
        }
        return dist;
    }

    std::vector<int32_t> connected_components() const {
        std::vector<int32_t> comp(V_, -1);
        for (uint32_t s = 0; s < V_; ++s) {
            if (comp[s] != -1) continue;
            comp[s] = static_cast<int32_t>(s);
            std::vector<uint32_t> q{s};
            while (!q.empty()) {
                uint32_t u = q.back(); q.pop_back();
                for (uint64_t i = offsets_[u]; i < offsets_[u+1]; ++i) {
                    uint32_t v = nbrs_[i];
                    if (comp[v] == -1) { comp[v] = static_cast<int32_t>(s); q.push_back(v); }
                }
            }
        }
        return comp;
    }

    std::vector<uint32_t> kcore() const {
        std::vector<uint32_t> deg(V_);
        for (uint32_t v = 0; v < V_; ++v) deg[v] = offsets_[v+1] - offsets_[v];
        uint32_t md = 0;
        for (uint32_t v = 0; v < V_; ++v) if (deg[v] > md) md = deg[v];
        std::vector<uint32_t> bin(md + 2, 0);
        for (uint32_t v = 0; v < V_; ++v) bin[deg[v]]++;
        uint32_t s = 0;
        for (uint32_t d = 0; d <= md; ++d) { uint32_t c = bin[d]; bin[d] = s; s += c; }
        std::vector<uint32_t> pos(V_), order(V_);
        for (uint32_t v = 0; v < V_; ++v) { pos[v] = bin[deg[v]]; order[pos[v]] = v; bin[deg[v]]++; }
        for (uint32_t d = md; d >= 1; --d) bin[d] = bin[d-1]; bin[0] = 0;
        std::vector<uint32_t> core(V_, 0);
        for (uint32_t i = 0; i < V_; ++i) {
            uint32_t v = order[i]; core[v] = deg[v];
            for (uint64_t j = offsets_[v]; j < offsets_[v+1]; ++j) {
                uint32_t u = nbrs_[j];
                if (deg[u] > deg[v]) {
                    uint32_t du = deg[u], pu = pos[u];
                    uint32_t pw = bin[du]; uint32_t w = order[pw];
                    if (u != w) { pos[u] = pw; pos[w] = pu; order[pu] = w; order[pw] = u; }
                    bin[du]++; deg[u]--;
                }
            }
        }
        return core;
    }

    uint64_t triangle_count() const {
        // Sort + dedupe per-vertex.
        std::vector<std::vector<uint32_t>> sorted(V_);
#ifdef BARRY_HAVE_OPENMP
        #pragma omp parallel for schedule(dynamic, 1024)
#endif
        for (uint32_t u = 0; u < V_; ++u) {
            sorted[u].assign(nbrs_.begin() + offsets_[u], nbrs_.begin() + offsets_[u+1]);
            std::sort(sorted[u].begin(), sorted[u].end());
            sorted[u].erase(std::unique(sorted[u].begin(), sorted[u].end()),
                            sorted[u].end());
        }

        uint64_t count = 0;
#ifdef BARRY_HAVE_OPENMP
        #pragma omp parallel for schedule(dynamic, 64) reduction(+:count)
#endif
        for (uint32_t u = 0; u < V_; ++u) {
            const auto& Nu = sorted[u];
            for (size_t i = 0; i < Nu.size(); ++i) {
                uint32_t v = Nu[i];
                if (v <= u) continue;
                const auto& Nv = sorted[v];
                size_t a = i + 1, b = 0;
                while (a < Nu.size() && b < Nv.size()) {
                    if (Nu[a] == Nv[b]) {
                        if (Nu[a] > v) ++count;
                        ++a; ++b;
                    } else if (Nu[a] < Nv[b]) ++a; else ++b;
                }
            }
        }
        return count;
    }

private:
    uint32_t              V_ = 0;
    std::vector<uint64_t> offsets_;
    std::vector<uint32_t> nbrs_;
};

}}  // namespace
