// A time-sorted CSR: the strongest simple baseline for time-respecting traversal,
// and the one every reviewer will ask about.
//
// Adjacency is grouped by source vertex and sorted by timestamp within each group,
// so expanding a frontier vertex u reached at time t is a binary search for the
// first out-edge with ts >= t followed by a forward scan. That is close to optimal
// for the query, with better locality than any pointer-chasing layout.
//
// What it cannot do is absorb a stream: the structure is built by a full sort, an
// arriving edge invalidates it, and out-of-order arrival breaks the ordering the
// binary search depends on. Those are the axes on which a streaming engine has to
// justify itself -- not query latency, where this baseline is genuinely excellent.

#pragma once

#include "barry/edge.hpp"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

namespace barry { namespace bench {

class TemporalCsr {
public:
    // Full rebuild from scratch: the only update path this structure has.
    void build(const Edge* edges, size_t n, uint32_t num_vertices) {
        v_ = num_vertices;
        offs_.assign(v_ + 1, 0);
        adj_.resize(n);

        for (size_t i = 0; i < n; ++i)
            if (edges[i].src < v_) ++offs_[edges[i].src + 1];
        for (uint32_t v = 0; v < v_; ++v) offs_[v + 1] += offs_[v];

        std::vector<uint64_t> cursor(offs_.begin(), offs_.end() - 1);
        for (size_t i = 0; i < n; ++i) {
            const Edge& e = edges[i];
            if (e.src >= v_) continue;
            adj_[cursor[e.src]++] = Entry{e.timestamp, e.dst};
        }
        // Sort each adjacency list by time so expansion can binary search.
        for (uint32_t v = 0; v < v_; ++v) {
            std::sort(adj_.begin() + offs_[v], adj_.begin() + offs_[v + 1],
                      [](const Entry& a, const Entry& b) { return a.ts < b.ts; });
        }
    }

    // Same semantics as Barry::forward_taint: earliest arrival time per reachable
    // vertex, sparse output, run to fixpoint.
    std::vector<std::pair<uint32_t, uint64_t>>
    forward_taint(uint32_t seed, uint64_t t_seed, uint64_t t_hi) const {
        std::vector<std::pair<uint32_t, uint64_t>> out;
        if (seed >= v_ || t_seed > t_hi) return out;

        std::unordered_map<uint32_t, uint64_t> first_seen;
        first_seen.reserve(1024);
        using QItem = std::pair<uint64_t, uint32_t>;
        std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> pq;
        first_seen.emplace(seed, t_seed);
        pq.emplace(t_seed, seed);

        while (!pq.empty()) {
            auto [t_u, u] = pq.top();
            pq.pop();
            auto it = first_seen.find(u);
            if (it == first_seen.end() || t_u > it->second) continue;
            out.emplace_back(u, t_u);

            // Binary search to the first out-edge at or after t_u, then scan while
            // still inside the horizon.
            uint64_t lo = offs_[u], hi = offs_[u + 1];
            uint64_t p = std::lower_bound(adj_.begin() + lo, adj_.begin() + hi, t_u,
                            [](const Entry& e, uint64_t t) { return e.ts < t; })
                         - adj_.begin();
            for (; p < hi && adj_[p].ts <= t_hi; ++p) {
                uint32_t d = adj_[p].dst;
                if (d >= v_) continue;
                auto [slot, inserted] = first_seen.try_emplace(d, adj_[p].ts);
                if (inserted || adj_[p].ts < slot->second) {
                    slot->second = adj_[p].ts;
                    pq.emplace(adj_[p].ts, d);
                }
            }
        }
        return out;
    }

    size_t bytes() const { return adj_.size() * sizeof(Entry) + offs_.size() * sizeof(uint64_t); }

    // ---- plain iteration, for storage-read benchmarking ----
    template <class F>
    void for_each_out(uint32_t u, F&& f) const {
        for (uint64_t p = offs_[u], e = offs_[u + 1]; p < e; ++p) f(adj_[p].dst);
    }
    // Time-windowed iteration: binary search to the first in-range entry, then a
    // contiguous scan that stops at the first entry past t_hi.
    template <class F>
    void for_each_out_in(uint32_t u, uint64_t t_lo, uint64_t t_hi, F&& f) const {
        uint64_t lo = offs_[u], hi = offs_[u + 1];
        uint64_t p = std::lower_bound(adj_.begin() + lo, adj_.begin() + hi, t_lo,
                        [](const Entry& e, uint64_t t) { return e.ts < t; })
                     - adj_.begin();
        for (; p < hi && adj_[p].ts <= t_hi; ++p) f(adj_[p].dst);
    }

private:
    struct Entry { uint64_t ts; uint32_t dst; };
    uint32_t              v_ = 0;
    std::vector<uint64_t> offs_;
    std::vector<Entry>    adj_;
};

}}  // namespace barry::bench
