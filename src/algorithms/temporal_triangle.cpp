#include "barry/engine.hpp"
#include <algorithm>

#ifdef BARRY_HAVE_OPENMP
#include <omp.h>
#endif

namespace barry {

// Temporal triangle (Paranjape et al., WSDM'17): triples (e1, e2, e3) forming
// a triangle on (u, v, w) with t1 < t2 < t3 and t3 - t1 <= delta.
uint64_t Barry::temporal_triangle_count(uint64_t t_lo,
                                        uint64_t t_hi,
                                        uint64_t delta) const {
    if (!vertex_index_) return 0;

    auto edges = query_range(t_lo, t_hi);
    uint64_t count = 0;

#ifdef BARRY_HAVE_OPENMP
    #pragma omp parallel for schedule(dynamic, 64) reduction(+:count)
#endif
    for (size_t i = 0; i < edges.size(); ++i) {
        const Edge& e1 = edges[i];
        uint64_t t1_max = std::min(e1.timestamp + delta, t_hi);

        // (e1.dst, w) with t2 in (e1.t, t1_max]
        auto e2_list = vertex_index_->neighbors_temporal(
            e1.dst, e1.timestamp + 1, t1_max);
        for (const Edge& e2 : e2_list) {
            if (e2.dst == e1.src) continue;
            // (e1.src, e2.dst) with t3 in (e2.t, t1_max]
            auto e3_list = vertex_index_->neighbors_temporal(
                e1.src, e2.timestamp + 1, t1_max);
            for (const Edge& e3 : e3_list) {
                if (e3.dst == e2.dst) {
                    ++count;
                    break;
                }
            }
        }
    }
    return count;
}

}  // namespace barry
