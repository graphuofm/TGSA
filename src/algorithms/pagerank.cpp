#include "barry/engine.hpp"
#include <algorithm>

#ifdef BARRY_HAVE_OPENMP
#include <omp.h>
#endif

namespace barry {

std::vector<float> Barry::pagerank(int max_iter, float damping) const {
    if (!vertex_index_) return {};
    const uint32_t V = vertex_index_->num_vertices();
    std::vector<float> pr(V, 1.0f / V);
    std::vector<float> next_pr(V);
    std::vector<uint32_t> out_deg(V);

    for (uint32_t u = 0; u < V; ++u) out_deg[u] = vertex_index_->out_degree(u);

    const float teleport = (1.0f - damping) / V;

    for (int iter = 0; iter < max_iter; ++iter) {
        std::fill(next_pr.begin(), next_pr.end(), teleport);

#ifdef BARRY_HAVE_OPENMP
        #pragma omp parallel
#endif
        {
            // Thread-local scratch reused across all assigned vertices.
            std::vector<uint32_t> nbr;
            nbr.reserve(64);

#ifdef BARRY_HAVE_OPENMP
            #pragma omp for schedule(dynamic, 1024)
#endif
            for (uint32_t u = 0; u < V; ++u) {
                if (out_deg[u] == 0) continue;
                float share = damping * pr[u] / out_deg[u];
                nbr.clear();
                vertex_index_->neighbors_dst_into(u, nbr);
                for (uint32_t v : nbr) {
                    if (v >= V) continue;
#ifdef BARRY_HAVE_OPENMP
                    #pragma omp atomic
#endif
                    next_pr[v] += share;
                }
            }
        }
        std::swap(pr, next_pr);
    }
    return pr;
}

}  // namespace barry
