// Log-reduction policies shared by bench_e8_attack (LANL) and bench_e14_tc
// (CADETS). Every policy is a pure function over the time-ordered edge stream
// seen through the (src, dst, t, class) view the benches already have: an
// optional pre-pass over the in-scope stream (`prepare`), then one `keep`
// decision per in-scope edge in stream order. The benches count reduction
// ratio and evidence survival over the edges `keep` accepts, exactly as before.
//
// Policies
//   full                 keep everything.
//   cpr        P         causality-preserving reduction: within a window of P
//                        time units a repeated (src,dst) pair is redundant; only
//                        the first occurrence per window is kept.
//   random     P         drop P percent of events uniformly at random.
//   loggc      P         LogGC-style garbage collection (Lee, Zhang, Xu, CCS'13).
//                        An entity v is TEMPORARY iff (a) it has at least one
//                        incoming edge, (b) its lifetime last(v)-first(v) over all
//                        incident edges is < P time units, and (c) no edge leaves
//                        v for a NON-temporary entity at or after v's last
//                        incoming edge. (c) is solved to a fixed point, so the
//                        temporary set is the whole dead-end subgraph: entities
//                        whose final state flows nowhere, or only into other
//                        temporary entities. Every edge incident to a temporary
//                        entity is removed. Approximation of the paper's rule:
//                        LogGC collects objects with no forward causal impact on
//                        live state, using create/unlink and exit to know which
//                        objects are dead; the benches see only flow edges, so a
//                        short lifetime stands in for "dead" and (c) stands in
//                        for "unreachable from live state".
//   loggc_reach P        reachability form of the same rule, closer to LogGC's
//                        definition of garbage (an event is garbage iff its
//                        effect reaches no live object). An entity is DEAD iff
//                        its lifetime is < P, LIVE otherwise. Edge u->v at t is
//                        removed iff v is dead and no time-respecting path
//                        leaving v at or after t reaches a live entity. Unlike
//                        `loggc`, a dead entity's out-flows into live state are
//                        kept. Solved exactly by one reverse-time sweep:
//                        R(v) = latest out-edge of v that is itself retained.
//   nodemerge  P         NodeMerge-style merging of read-only initialization
//                        (Tang et al., CCS'18). A source entity u is READ-ONLY
//                        iff it has zero incoming edges in the whole stream
//                        (it is never written). For each reader p, edges u->p
//                        with u read-only that fall within the first P time
//                        units of p's lifetime (its first incident edge, either
//                        direction) are collapsed to the first event per (u,p).
//                        Edges from written entities, and initialization reads
//                        after the window, are kept. In the benches' flow view a
//                        read is object->subject, so the reader is the edge's
//                        dst and the read-only file is its src; NodeMerge's
//                        "process initialization" is the reader's first P units.
//   dedup                keep the first event per (src,dst) over the whole
//                        stream: the one-edge-per-pair model of STINGER and
//                        Sortledton, and the limit of cpr as P -> infinity. P
//                        is ignored.
//   sample_pa:D  P       provenance-aware sampling: drop P percent uniformly at
//                        random, except that every edge incident to a HUB is
//                        kept, a hub being an entity with more than D distinct
//                        neighbours (in and out, over the whole in-scope stream).
//
// `full`, `cpr` and `random` reproduce the decisions of the former inline loops
// byte for byte: the same RNG type, seed and draw order.

#pragma once

#include "barry/edge.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace barry::bench::reduction {

using InScope = std::function<bool(const Edge&)>;

struct Policy {
    virtual ~Policy() = default;
    // Pre-pass over the stream. `in_scope` is the class filter the bench applies
    // before any policy decision; a policy must look at exactly those edges.
    virtual void prepare(const Edge* /*edges*/, size_t /*n*/, const InScope& /*in_scope*/) {}
    // One call per in-scope edge, in stream order. True = retain.
    virtual bool keep(const Edge& e) = 0;
    // Name as it should appear in the CSV `policy` column.
    virtual std::string name() const = 0;
};

inline uint64_t pair_key(uint32_t s, uint32_t d) {
    return (static_cast<uint64_t>(s) << 32) | d;
}

// ------------------------------------------------------------------- full
struct Full final : Policy {
    bool keep(const Edge&) override { return true; }
    std::string name() const override { return "full"; }
};

// ------------------------------------------------------------------- cpr
struct Cpr final : Policy {
    double param;
    std::unordered_map<uint64_t, uint64_t> last_seen;   // (src,dst) -> window index
    explicit Cpr(double p) : param(p) {}
    bool keep(const Edge& e) override {
        if (!(param > 0)) return true;
        uint64_t key = pair_key(e.src, e.dst);
        uint64_t win = e.timestamp / static_cast<uint64_t>(param);
        auto it = last_seen.find(key);
        if (it != last_seen.end() && it->second == win) return false;   // redundant
        last_seen[key] = win;
        return true;
    }
    std::string name() const override { return "cpr"; }
};

// ------------------------------------------------------------------- random
struct Random final : Policy {
    double param;
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    Random(double p, uint64_t seed) : param(p), rng(seed) {}
    bool keep(const Edge&) override {
        if (!(param > 0)) return true;
        return !(unit(rng) < param / 100.0);
    }
    std::string name() const override { return "random"; }
};

// ------------------------------------------------------------- vertex scan
struct VertexSpan {
    uint64_t first = ~0ULL, last = 0;     // any direction
    uint64_t last_in = 0, last_out = 0;   // per direction
    bool has_in = false, has_out = false;
};

inline std::vector<VertexSpan> scan_spans(const Edge* edges, size_t n, const InScope& in_scope) {
    uint32_t v_max = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!in_scope(edges[i])) continue;
        v_max = std::max(v_max, std::max(edges[i].src, edges[i].dst));
    }
    std::vector<VertexSpan> sp(static_cast<size_t>(v_max) + 1);
    for (size_t i = 0; i < n; ++i) {
        const Edge& e = edges[i];
        if (!in_scope(e)) continue;
        VertexSpan& s = sp[e.src];
        VertexSpan& d = sp[e.dst];
        s.first = std::min(s.first, e.timestamp); s.last = std::max(s.last, e.timestamp);
        d.first = std::min(d.first, e.timestamp); d.last = std::max(d.last, e.timestamp);
        s.has_out = true; s.last_out = std::max(s.last_out, e.timestamp);
        d.has_in  = true; d.last_in  = std::max(d.last_in,  e.timestamp);
    }
    return sp;
}

// ------------------------------------------------------------------- loggc
struct LogGC final : Policy {
    double param;
    std::vector<uint8_t> temporary;
    size_t n_temporary = 0, passes = 0;
    explicit LogGC(double p) : param(p) {}
    void prepare(const Edge* edges, size_t n, const InScope& in_scope) override {
        if (!(param > 0)) return;
        std::vector<VertexSpan> sp = scan_spans(edges, n, in_scope);
        temporary.assign(sp.size(), 0);
        const uint64_t life = static_cast<uint64_t>(param);
        std::vector<uint8_t> candidate(sp.size(), 0);
        for (size_t v = 0; v < sp.size(); ++v) {
            const VertexSpan& s = sp[v];
            if (s.has_in && s.last - s.first < life) candidate[v] = 1;  // (a) and (b)
        }
        // (c) to a fixed point. live_out[v] = latest out-edge of v to an entity
        // not (yet) known temporary. The temporary set only grows, so each pass
        // can only lower live_out, and the iteration is monotone.
        std::vector<uint64_t> live_out(sp.size());
        for (;;) {
            std::fill(live_out.begin(), live_out.end(), 0);
            std::vector<uint8_t> has_live_out(sp.size(), 0);
            for (size_t i = 0; i < n; ++i) {
                const Edge& e = edges[i];
                if (!in_scope(e) || !candidate[e.src] || temporary[e.dst]) continue;
                has_live_out[e.src] = 1;
                live_out[e.src] = std::max(live_out[e.src], e.timestamp);
            }
            size_t added = 0;
            for (size_t v = 0; v < sp.size(); ++v) {
                if (!candidate[v] || temporary[v]) continue;
                if (has_live_out[v] && live_out[v] >= sp[v].last_in) continue;
                temporary[v] = 1; ++n_temporary; ++added;
            }
            ++passes;
            if (added == 0) break;
        }
        std::fprintf(stderr, "[loggc] lifetime<%g: %zu/%zu entities temporary (%zu passes)\n",
                     param, n_temporary, sp.size(), passes);
    }
    bool keep(const Edge& e) override {
        if (!(param > 0)) return true;
        return !(temporary[e.src] || temporary[e.dst]);
    }
    std::string name() const override { return "loggc"; }
};

// ------------------------------------------------------------- loggc_reach
struct LogGCReach final : Policy {
    double param;
    std::vector<uint8_t>  dead;
    std::vector<uint8_t>  has_r;   // dead v has a retained out-edge
    std::vector<uint64_t> r;       // latest retained out-edge of dead v
    explicit LogGCReach(double p) : param(p) {}
    bool retained(const Edge& e) const {   // valid once R is final, or for edges later than e
        return !dead[e.dst] || (has_r[e.dst] && r[e.dst] >= e.timestamp);
    }
    void prepare(const Edge* edges, size_t n, const InScope& in_scope) override {
        if (!(param > 0)) return;
        std::vector<VertexSpan> sp = scan_spans(edges, n, in_scope);
        const uint64_t life = static_cast<uint64_t>(param);
        dead.assign(sp.size(), 0); has_r.assign(sp.size(), 0); r.assign(sp.size(), 0);
        size_t n_dead = 0;
        for (size_t v = 0; v < sp.size(); ++v)
            if (sp[v].first != ~0ULL && sp[v].last - sp[v].first < life) { dead[v] = 1; ++n_dead; }
        // Visit in-scope edges in descending time. The stream is time-ordered up
        // to a few out-of-order records (CADETS: 179), so sort an index only
        // when it is not already sorted.
        std::vector<uint32_t> order;
        bool sorted = true;
        uint64_t prev = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!in_scope(edges[i])) continue;
            if (edges[i].timestamp < prev) { sorted = false; break; }
            prev = edges[i].timestamp;
        }
        if (!sorted) {
            for (size_t i = 0; i < n; ++i) if (in_scope(edges[i])) order.push_back(static_cast<uint32_t>(i));
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                return edges[a].timestamp < edges[b].timestamp; });
        }
        const size_t m = sorted ? n : order.size();
        auto at = [&](size_t k) -> const Edge& { return edges[sorted ? k : order[k]]; };
        // Equal timestamps form a group; iterate the group to a fixed point so a
        // same-instant chain u->v->w is resolved regardless of record order.
        size_t hi = m;
        while (hi > 0) {
            size_t lo = hi - 1;
            const uint64_t t = at(lo).timestamp;
            while (lo > 0 && at(lo - 1).timestamp == t) --lo;
            for (bool changed = true; changed;) {
                changed = false;
                for (size_t k = lo; k < hi; ++k) {
                    const Edge& e = at(k);
                    if (sorted && !in_scope(e)) continue;
                    if (!dead[e.src] || (has_r[e.src] && r[e.src] >= t)) continue;
                    if (retained(e)) { has_r[e.src] = 1; r[e.src] = t; changed = true; }
                }
            }
            hi = lo;
        }
        std::fprintf(stderr, "[loggc_reach] lifetime<%g: %zu/%zu entities dead%s\n",
                     param, n_dead, sp.size(), sorted ? "" : " (index sorted)");
    }
    bool keep(const Edge& e) override {
        if (!(param > 0)) return true;
        return retained(e);
    }
    std::string name() const override { return "loggc_reach"; }
};

// --------------------------------------------------------------- nodemerge
struct NodeMerge final : Policy {
    double param;
    std::vector<uint8_t>  read_only;     // zero in-degree over the whole stream
    std::vector<uint64_t> first_seen;    // reader's first incident edge
    std::unordered_set<uint64_t> merged; // (u,p) pairs already represented
    explicit NodeMerge(double p) : param(p) {}
    void prepare(const Edge* edges, size_t n, const InScope& in_scope) override {
        if (!(param > 0)) return;
        std::vector<VertexSpan> sp = scan_spans(edges, n, in_scope);
        read_only.assign(sp.size(), 0);
        first_seen.assign(sp.size(), 0);
        size_t n_ro = 0;
        for (size_t v = 0; v < sp.size(); ++v) {
            first_seen[v] = sp[v].first;
            if (sp[v].has_out && !sp[v].has_in) { read_only[v] = 1; ++n_ro; }
        }
        std::fprintf(stderr, "[nodemerge] window %g: %zu/%zu entities read-only\n",
                     param, n_ro, sp.size());
    }
    bool keep(const Edge& e) override {
        if (!(param > 0)) return true;
        if (!read_only[e.src]) return true;                               // a written entity: not an init read
        if (e.timestamp >= first_seen[e.dst] + static_cast<uint64_t>(param)) return true;  // past initialization
        return merged.insert(pair_key(e.src, e.dst)).second;              // first event per (u,p) survives
    }
    std::string name() const override { return "nodemerge"; }
};

// ------------------------------------------------------------------- dedup
struct Dedup final : Policy {
    std::unordered_set<uint64_t> seen;
    bool keep(const Edge& e) override { return seen.insert(pair_key(e.src, e.dst)).second; }
    std::string name() const override { return "dedup"; }
};

// --------------------------------------------------------------- sample_pa
struct SamplePA final : Policy {
    double param;            // drop percentage
    uint64_t hub_degree;     // distinct-neighbour degree above which an entity is a hub
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    std::vector<uint8_t> hub;
    SamplePA(double p, uint64_t d, uint64_t seed) : param(p), hub_degree(d), rng(seed) {}
    void prepare(const Edge* edges, size_t n, const InScope& in_scope) override {
        if (!(param > 0)) return;
        uint32_t v_max = 0;
        std::unordered_set<uint64_t> pairs;
        for (size_t i = 0; i < n; ++i) {
            const Edge& e = edges[i];
            if (!in_scope(e)) continue;
            v_max = std::max(v_max, std::max(e.src, e.dst));
            pairs.insert(pair_key(std::min(e.src, e.dst), std::max(e.src, e.dst)));
        }
        std::vector<uint64_t> deg(static_cast<size_t>(v_max) + 1, 0);
        for (uint64_t k : pairs) { ++deg[k >> 32]; ++deg[k & 0xffffffffULL]; }
        hub.assign(deg.size(), 0);
        size_t n_hub = 0;
        for (size_t v = 0; v < deg.size(); ++v) if (deg[v] > hub_degree) { hub[v] = 1; ++n_hub; }
        std::fprintf(stderr, "[sample_pa] degree>%llu: %zu/%zu entities are hubs\n",
                     (unsigned long long)hub_degree, n_hub, deg.size());
    }
    bool keep(const Edge& e) override {
        if (!(param > 0)) return true;
        if (hub[e.src] || hub[e.dst]) return true;   // hubs are never sampled
        return !(unit(rng) < param / 100.0);
    }
    std::string name() const override { return "sample_pa:" + std::to_string(hub_degree); }
};

// --------------------------------------------------------------- factory
// `name` is the --policy string. sample_pa takes its hub threshold after a
// colon ("sample_pa:1000") so the CSV `policy` column stays self-describing
// and `param` keeps its one meaning per policy.
inline std::unique_ptr<Policy> make_policy(const std::string& name, double param, uint64_t rng_seed) {
    if (name == "full")      return std::make_unique<Full>();
    if (name == "cpr")       return std::make_unique<Cpr>(param);
    if (name == "random")    return std::make_unique<Random>(param, rng_seed);
    if (name == "loggc")     return std::make_unique<LogGC>(param);
    if (name == "loggc_reach") return std::make_unique<LogGCReach>(param);
    if (name == "nodemerge") return std::make_unique<NodeMerge>(param);
    if (name == "dedup")     return std::make_unique<Dedup>();
    if (name.rfind("sample_pa", 0) == 0) {
        uint64_t d = 0;
        size_t colon = name.find(':');
        if (colon != std::string::npos) d = std::strtoull(name.c_str() + colon + 1, nullptr, 10);
        return std::make_unique<SamplePA>(param, d, rng_seed);
    }
    std::fprintf(stderr, "unknown policy '%s' (full|cpr|random|loggc|loggc_reach|nodemerge|dedup|sample_pa:D)\n",
                 name.c_str());
    return nullptr;
}

}  // namespace barry::bench::reduction
