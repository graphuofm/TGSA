#pragma once

// E33 aligned comparison matrix: the q1, q3 and q4 phases on GraphOne's
// archived adjacency (docs/E33_SPEC.md in the Barry repo is the authority;
// q2 is analytics/window_query.h, job 28).
//
// Added for the cross-system comparison, not part of upstream GraphOne. The
// formulas are the spec's, character for character, so a run that answered a
// different question cannot pass: its checksum would differ from the .ref the
// in-process harness writes.
//
// Every kernel reads through a consistent (non-STALE) static view taken after
// Make Graph has caught up, so the whole corpus sits in the delta chains and
// the non-archived tail is empty; the tail is still scanned, as job 28 does,
// so an unarchived remainder would be counted rather than dropped.
//
// Query file: GNOMON_QUERY_FILE, records of three little-endian u64 {v, lo, hi}.
// Budget:     GNOMON_PHASE_BUDGET_SEC (default 1800). The first 20 queries are
//             timed, the total is projected, and only as many queries as fit
//             are run; "<Phase> Done = k/N" says how many.
//
// Output lines (parsed by scripts/run_graphone.py):
//   job 34  Scan Query Time / Scan Count / Scan Checksum / Scan Threads
//   job 35  Reach Query Time / Reach Count / Reach Checksum / Reach Median us /
//           Reach P95 us / Reach Done
//   job 36  Asof Query Time / Asof Count / Asof Checksum / Asof Median us /
//           Asof P95 us / Asof Done

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

struct e33_query_t { uint64_t v, lo, hi; };

inline bool e33_read_queries(std::vector<e33_query_t>& queries)
{
    const char* qpath = getenv("GNOMON_QUERY_FILE");
    if (!qpath) {
        cout << "GNOMON_QUERY_FILE not set" << endl;
        return false;
    }
    FILE* qf = fopen(qpath, "rb");
    if (!qf) { perror(qpath); return false; }
    e33_query_t q;
    while (fread(&q, sizeof(q), 1, qf) == 1) queries.push_back(q);
    fclose(qf);
    return !queries.empty();
}

inline double e33_budget_sec()
{
    const char* s = getenv("GNOMON_PHASE_BUDGET_SEC");
    double b = s ? atof(s) : 0.0;
    return b > 0 ? b : 1800.0;
}

inline double e33_now()
{
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

// Same interpolating percentile as benchmark/common.hpp in the Barry repo.
inline double e33_percentile(std::vector<double>& xs, double q)
{
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    double pos = q * (xs.size() - 1);
    size_t lo = (size_t)pos;
    size_t hi = std::min(lo + 1, xs.size() - 1);
    double f = pos - lo;
    return xs[lo] + (xs[hi] - xs[lo]) * f;
}

// Budgeted query loop. Calls one(i) for query i and times it; after the first
// 20 queries the total is projected and the loop is cut to what fits. Returns
// the number of queries actually run; latencies in microseconds are left in
// lat_us, the wall time of the loop in *total_sec.
template <class F>
inline size_t e33_run_budgeted(size_t n, double budget, std::vector<double>& lat_us,
                               double* total_sec, F&& one)
{
    lat_us.clear();
    lat_us.reserve(n);
    size_t k = n;
    const double start = e33_now();
    size_t i = 0;
    for (; i < k; ++i) {
        const double t0 = e33_now();
        one(i);
        const double t1 = e33_now();
        lat_us.push_back((t1 - t0) * 1e6);
        if (i + 1 == 20 && n > 20) {
            const double per = (t1 - start) / 20.0;
            if (per * (double)n > budget) {
                size_t fit = (size_t)(budget / per);
                k = std::max<size_t>(20, std::min(n, fit));
            }
        }
        if (t1 - start > budget) { ++i; break; }
    }
    *total_sec = e33_now() - start;
    return i;
}

// Walk every archived out-event of v in the snapshot and call fn(dst, ts).
// This is job 28's delta-chain walk, factored out.
template <class T, class F>
inline void e33_for_each_out(gview_t<T>* snaph, vid_t v, F&& fn)
{
    delta_adjlist_t<T>* delta_adjlist = snaph->get_nebrs_archived_out(v);
    if (0 == delta_adjlist) return;
    degree_t delta_degree = snaph->get_degree_out(v);
    while (delta_adjlist != 0 && delta_degree > 0) {
        T* local_adjlist = delta_adjlist->get_adjlist();
        degree_t local_degree = delta_adjlist->get_nebrcount();
        degree_t i_count = min(local_degree, delta_degree);
        for (degree_t k = 0; k < i_count; ++k) {
            fn((uint64_t)get_sid(local_adjlist[k]),
               (uint64_t)local_adjlist[k].second.value_sid);
        }
        delta_adjlist = delta_adjlist->get_next();
        delta_degree -= local_degree;
    }
}

// Out-events of v still in the log rather than the adjacency. Empty after a
// full archive; scanned so nothing is silently dropped if it were not.
template <class T, class F>
inline void e33_for_each_tail_out(edgeT_t<T>* tail, index_t tail_count, vid_t v, F&& fn)
{
    for (index_t j = 0; j < tail_count; ++j) {
        if (TO_SID(tail[j].src_id) != v) continue;
        fn((uint64_t)get_sid(tail[j].dst_id),
           (uint64_t)tail[j].dst_id.second.value_sid);
    }
}

// ------------------------------------------------------------------ q1, job 34
// Window scan over the whole adjacency: every event with lo <= ts <= hi where
// lo = t_max - w, hi = t_max, w = max(1, (t_max - t_min) / 100). GraphOne has
// no time index, so the scan visits every vertex's delta chain; it runs on
// THD_COUNT OpenMP threads, GraphOne's own analytics convention. An untimed
// pre-pass finds t_min and t_max, which the engine does not keep.
template <class T>
void mem_scan_query(gview_t<T>* snaph)
{
    const vid_t v_count = snaph->get_vcount();
    edgeT_t<T>* tail = 0;
    index_t tail_count = snaph->get_nonarchived_edges(tail);

    uint64_t t_min = UINT64_MAX, t_max = 0;
    double pre0 = e33_now();
    #pragma omp parallel num_threads(THD_COUNT)
    {
        uint64_t lmin = UINT64_MAX, lmax = 0;
        #pragma omp for schedule(dynamic, 4096) nowait
        for (vid_t v = 0; v < v_count; ++v) {
            e33_for_each_out<T>(snaph, v, [&](uint64_t, uint64_t ts) {
                if (ts < lmin) lmin = ts;
                if (ts > lmax) lmax = ts;
            });
        }
        #pragma omp critical
        {
            if (lmin < t_min) t_min = lmin;
            if (lmax > t_max) t_max = lmax;
        }
    }
    for (index_t j = 0; j < tail_count; ++j) {
        uint64_t ts = (uint64_t)tail[j].dst_id.second.value_sid;
        if (ts < t_min) t_min = ts;
        if (ts > t_max) t_max = ts;
    }
    double pre1 = e33_now();
    if (t_min == UINT64_MAX) { t_min = 0; t_max = 0; }

    uint64_t w = (t_max - t_min) / 100;
    if (w < 1) w = 1;
    const uint64_t lo = t_max - w, hi = t_max;
    cout << "Scan t_min = " << t_min << endl;
    cout << "Scan t_max = " << t_max << endl;
    cout << "Scan w = " << w << endl;
    cout << "Scan Prepass Time = " << pre1 - pre0 << endl;

    uint64_t count = 0, checksum = 0;
    double start = e33_now();
    #pragma omp parallel num_threads(THD_COUNT) reduction(+:count, checksum)
    {
        #pragma omp for schedule(dynamic, 4096)
        for (vid_t v = 0; v < v_count; ++v) {
            const uint64_t src = (uint64_t)v;
            e33_for_each_out<T>(snaph, v, [&](uint64_t dst, uint64_t ts) {
                if (ts >= lo && ts <= hi) {
                    checksum += ts ^ (dst << 32) ^ src;
                    ++count;
                }
            });
        }
    }
    for (index_t j = 0; j < tail_count; ++j) {
        uint64_t ts = (uint64_t)tail[j].dst_id.second.value_sid;
        if (ts >= lo && ts <= hi) {
            checksum += ts ^ ((uint64_t)get_sid(tail[j].dst_id) << 32)
                           ^ (uint64_t)TO_SID(tail[j].src_id);
            ++count;
        }
    }
    double end = e33_now();

    cout << "Scan Threads = " << THD_COUNT << endl;
    cout << "Scan Query Time = " << end - start << endl;
    cout << "Scan Count = " << count << endl;
    cout << "Scan Checksum = " << checksum << endl;
}

// ------------------------------------------------------------------ q3, job 35
// Time-respecting earliest-arrival reachability from v inside [lo, hi].
// a(v) = lo; an out-event (u -> d, ts) is usable when a(u) <= ts <= hi and
// improves d when d is unlabeled or ts < a(d). The fixpoint is unique, so
// the visit order is free: vertices are popped in increasing a() order
// (Dijkstra-style, stale heap entries skipped), which scans each reached
// vertex's neighbourhood once per query. Labels live in a per-query hash map.
// reached = labeled d != v over all queries; chk = sum of ((u64)d ^ a(d)).
template <class T>
void mem_reach_query(gview_t<T>* snaph)
{
    std::vector<e33_query_t> queries;
    if (!e33_read_queries(queries)) return;
    cout << "reach queries = " << queries.size() << endl;

    edgeT_t<T>* tail = 0;
    index_t tail_count = snaph->get_nonarchived_edges(tail);

    uint64_t reached = 0, checksum = 0;
    std::unordered_map<uint64_t, uint64_t> label;
    typedef std::pair<uint64_t, uint64_t> item_t;  // (a(u), u)
    std::priority_queue<item_t, std::vector<item_t>, std::greater<item_t> > heap;

    std::vector<double> lat_us;
    double total = 0;
    size_t done = e33_run_budgeted(queries.size(), e33_budget_sec(), lat_us, &total,
        [&](size_t i) {
            const uint64_t v = queries[i].v, lo = queries[i].lo, hi = queries[i].hi;
            label.clear();
            label.emplace(v, lo);
            heap.push(item_t(lo, v));
            while (!heap.empty()) {
                const item_t top = heap.top();
                heap.pop();
                const uint64_t au = top.first, u = top.second;
                if (au != label[u]) continue;  // superseded by a smaller label
                auto relax = [&](uint64_t d, uint64_t ts) {
                    if (ts < au || ts > hi) return;
                    auto it = label.find(d);
                    if (it == label.end()) {
                        label.emplace(d, ts);
                        heap.push(item_t(ts, d));
                    } else if (ts < it->second) {
                        it->second = ts;
                        heap.push(item_t(ts, d));
                    }
                };
                e33_for_each_out<T>(snaph, (vid_t)u, relax);
                if (tail_count) e33_for_each_tail_out<T>(tail, tail_count, (vid_t)u, relax);
            }
            for (auto it = label.begin(); it != label.end(); ++it) {
                if (it->first == v) continue;
                ++reached;
                checksum += it->first ^ it->second;
            }
        });

    cout << "Reach Query Time = " << total << endl;
    cout << "Reach Count = " << reached << endl;
    cout << "Reach Checksum = " << checksum << endl;
    cout << "Reach Median us = " << e33_percentile(lat_us, 0.5) << endl;
    cout << "Reach P95 us = " << e33_percentile(lat_us, 0.95) << endl;
    cout << "Reach Done = " << done << "/" << queries.size() << endl;
}

// ------------------------------------------------------------------ q4, job 36
// As-of neighbours: for each distinct dst among out-events of v with ts <= t
// (t = hi), the maximum such ts. pairs = distinct dsts over all queries;
// chk = sum of (((u64)dst << 20) ^ maxts ^ (u64)v).
template <class T>
void mem_asof_query(gview_t<T>* snaph)
{
    std::vector<e33_query_t> queries;
    if (!e33_read_queries(queries)) return;
    cout << "asof queries = " << queries.size() << endl;

    edgeT_t<T>* tail = 0;
    index_t tail_count = snaph->get_nonarchived_edges(tail);

    uint64_t pairs = 0, checksum = 0;
    std::unordered_map<uint64_t, uint64_t> latest;

    std::vector<double> lat_us;
    double total = 0;
    size_t done = e33_run_budgeted(queries.size(), e33_budget_sec(), lat_us, &total,
        [&](size_t i) {
            const uint64_t v = queries[i].v, t = queries[i].hi;
            latest.clear();
            auto visit = [&](uint64_t d, uint64_t ts) {
                if (ts > t) return;
                auto it = latest.find(d);
                if (it == latest.end()) latest.emplace(d, ts);
                else if (ts > it->second) it->second = ts;
            };
            e33_for_each_out<T>(snaph, (vid_t)v, visit);
            if (tail_count) e33_for_each_tail_out<T>(tail, tail_count, (vid_t)v, visit);
            pairs += latest.size();
            for (auto it = latest.begin(); it != latest.end(); ++it) {
                checksum += (it->first << 20) ^ it->second ^ v;
            }
        });

    cout << "Asof Query Time = " << total << endl;
    cout << "Asof Count = " << pairs << endl;
    cout << "Asof Checksum = " << checksum << endl;
    cout << "Asof Median us = " << e33_percentile(lat_us, 0.5) << endl;
    cout << "Asof P95 us = " << e33_percentile(lat_us, 0.95) << endl;
    cout << "Asof Done = " << done << "/" << queries.size() << endl;
}
