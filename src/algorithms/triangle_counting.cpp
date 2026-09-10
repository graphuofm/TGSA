#include "barry/engine.hpp"
#include <algorithm>

#ifdef BARRY_HAVE_OPENMP
#include <omp.h>
#endif

namespace barry {

uint64_t Barry::triangle_count() const {
    if (!vertex_index_) return 0;
    const uint32_t V = vertex_index_->num_vertices();
    uint64_t count = 0;

#ifdef BARRY_HAVE_OPENMP
    #pragma omp parallel reduction(+:count)
#endif
    {
        std::vector<uint32_t> Nu, Nv;
        Nu.reserve(64);
        Nv.reserve(64);

#ifdef BARRY_HAVE_OPENMP
        #pragma omp for schedule(dynamic, 64)
#endif
        for (uint32_t u = 0; u < V; ++u) {
            Nu.clear();
            vertex_index_->sorted_neighbors_unique_into(u, Nu);
            for (size_t i = 0; i < Nu.size(); ++i) {
                uint32_t v = Nu[i];
                if (v <= u) continue;

                Nv.clear();
                vertex_index_->sorted_neighbors_unique_into(v, Nv);
                size_t a = i + 1, b = 0;
                while (a < Nu.size() && b < Nv.size()) {
                    if (Nu[a] == Nv[b]) {
                        if (Nu[a] > v) ++count;
                        ++a; ++b;
                    } else if (Nu[a] < Nv[b]) {
                        ++a;
                    } else {
                        ++b;
                    }
                }
            }
        }
    }
    return count;
}

}  // namespace barry
