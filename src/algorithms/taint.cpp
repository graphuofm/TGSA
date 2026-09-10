#include "barry/engine.hpp"
#include <queue>
#include <unordered_map>
#include <utility>

namespace barry {

namespace {
using QItem = std::pair<uint64_t, uint32_t>;  // (time, vertex)
constexpr size_t kFrontierReserve = 1024;
}  // namespace

// P2 -- forward taint propagation.
//
// Dijkstra over the time-respecting expansion: popping a vertex yields its
// earliest possible taint time, so each vertex is emitted exactly once. The
// visited map is sparse, so a query that taints k vertices costs O(k log k +
// sum-of-degrees) rather than the O(V) that a dense distance array would force
// on every call.
std::vector<Barry::TaintNode> Barry::forward_taint(uint32_t seed,
                                                   uint64_t t_seed,
                                                   uint64_t t_hi,
                                                   size_t   max_nodes) const {
    std::vector<TaintNode> out;
    if (!vertex_index_) return out;
    const uint32_t V = vertex_index_->num_vertices();
    if (seed >= V || t_seed > t_hi) return out;

    std::unordered_map<uint32_t, uint64_t> first_seen;
    first_seen.reserve(kFrontierReserve);
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> pq;
    std::vector<TemporalRef> scratch;      // reused across every expansion
    scratch.reserve(256);

    first_seen.emplace(seed, t_seed);
    pq.emplace(t_seed, seed);

    while (!pq.empty()) {
        auto [t_u, u] = pq.top();
        pq.pop();

        auto it = first_seen.find(u);
        if (it == first_seen.end() || t_u > it->second) continue;  // stale entry

        out.push_back(TaintNode{u, t_u});
        if (max_nodes && out.size() >= max_nodes) break;

        // Timestamps are cached in the index, so this scans the bucket directly
        // and never dereferences into the Arena.
        scratch.clear();
        vertex_index_->neighbors_temporal_into(u, t_u, t_hi, scratch);
        for (const TemporalRef& r : scratch) {
            if (r.peer >= V) continue;
            auto [slot, inserted] = first_seen.try_emplace(r.peer, r.ts);
            if (inserted || r.ts < slot->second) {
                slot->second = r.ts;
                pq.emplace(r.ts, r.peer);
            }
        }
    }
    return out;
}

// P1 -- backward causality trace (BackTracker semantics).
//
// Mirror image of forward_taint: walk in-edges with non-increasing timestamps
// and keep, for each ancestor, the LATEST time at which it could still have
// propagated influence forward to the alert. Requires the reverse index.
std::vector<Barry::TaintNode> Barry::backward_trace(uint32_t alert,
                                                    uint64_t t_alert,
                                                    uint64_t t_lo,
                                                    size_t   max_nodes) const {
    std::vector<TaintNode> out;
    if (!vertex_index_ || !vertex_index_->has_reverse()) return out;
    const uint32_t V = vertex_index_->num_vertices();
    if (alert >= V || t_lo > t_alert) return out;

    std::unordered_map<uint32_t, uint64_t> last_seen;
    last_seen.reserve(kFrontierReserve);
    std::priority_queue<QItem> pq;  // max-heap: latest influence time first
    std::vector<TemporalRef> scratch;
    scratch.reserve(256);

    last_seen.emplace(alert, t_alert);
    pq.emplace(t_alert, alert);

    while (!pq.empty()) {
        auto [t_u, u] = pq.top();
        pq.pop();

        auto it = last_seen.find(u);
        if (it == last_seen.end() || t_u < it->second) continue;  // stale entry

        out.push_back(TaintNode{u, t_u});
        if (max_nodes && out.size() >= max_nodes) break;

        scratch.clear();
        vertex_index_->in_neighbors_temporal_into(u, t_lo, t_u, scratch);
        for (const TemporalRef& r : scratch) {
            if (r.peer >= V) continue;
            auto [slot, inserted] = last_seen.try_emplace(r.peer, r.ts);
            if (inserted || r.ts > slot->second) {
                slot->second = r.ts;
                pq.emplace(r.ts, r.peer);
            }
        }
    }
    return out;
}

}  // namespace barry
