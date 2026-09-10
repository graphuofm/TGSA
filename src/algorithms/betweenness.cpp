#include "barry/engine.hpp"
#include <cstdlib>
#include <queue>
#include <stack>

#ifdef BARRY_HAVE_OPENMP
#include <omp.h>
#endif

namespace barry {

std::vector<float> Barry::betweenness_centrality(int num_samples) const {
    if (!vertex_index_) return {};
    const uint32_t V = vertex_index_->num_vertices();
    const bool   sample = (num_samples > 0);
    const size_t total  = sample ? static_cast<size_t>(num_samples) : V;

    std::vector<float> bc(V, 0.0f);

#ifdef BARRY_HAVE_OPENMP
    #pragma omp parallel
#endif
    {
        std::vector<float> local_bc(V, 0.0f);

#ifdef BARRY_HAVE_OPENMP
        #pragma omp for schedule(dynamic, 1)
#endif
        for (size_t s_idx = 0; s_idx < total; ++s_idx) {
            uint32_t s = sample ? static_cast<uint32_t>(std::rand() % V)
                                : static_cast<uint32_t>(s_idx);

            std::vector<int32_t> dist(V, -1);
            std::vector<float>   sigma(V, 0.0f);
            std::vector<std::vector<uint32_t>> pred(V);
            std::stack<uint32_t> S;

            dist[s]  = 0;
            sigma[s] = 1.0f;

            std::queue<uint32_t> Q;
            Q.push(s);
            std::vector<uint32_t> nbr;
            nbr.reserve(64);
            while (!Q.empty()) {
                uint32_t v = Q.front(); Q.pop();
                S.push(v);
                nbr.clear();
                vertex_index_->neighbors_dst_into(v, nbr);
                for (uint32_t w : nbr) {
                    if (w >= V) continue;
                    if (dist[w] < 0) {
                        dist[w] = dist[v] + 1;
                        Q.push(w);
                    }
                    if (dist[w] == dist[v] + 1) {
                        sigma[w] += sigma[v];
                        pred[w].push_back(v);
                    }
                }
            }

            std::vector<float> delta(V, 0.0f);
            while (!S.empty()) {
                uint32_t w = S.top(); S.pop();
                for (uint32_t v : pred[w]) {
                    delta[v] += (sigma[v] / sigma[w]) * (1.0f + delta[w]);
                }
                if (w != s) local_bc[w] += delta[w];
            }
        }

#ifdef BARRY_HAVE_OPENMP
        #pragma omp critical
#endif
        {
            for (uint32_t v = 0; v < V; ++v) bc[v] += local_bc[v];
        }
    }

    if (sample) {
        float scale = static_cast<float>(V) / num_samples;
        for (auto& b : bc) b *= scale;
    }
    return bc;
}

}  // namespace barry
