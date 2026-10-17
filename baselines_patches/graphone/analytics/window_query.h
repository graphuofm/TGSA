#pragma once

// Windowed neighbour delivery on GraphOne's archived adjacency.
//
// Added for a cross-system comparison, not part of upstream GraphOne. The
// queries are read from a file written by the Gnomon harness, so both systems
// answer the identical (vertex, t_lo, t_hi) list, and both accumulate the same
// checksum over delivered (dst, timestamp) pairs. A faster time that answered a
// different question therefore cannot pass: the checksums would differ.
//
// The access pattern is the one an entity-keyed store is obliged to use. The
// adjacency is grouped by source vertex and ordered by arrival within the delta
// chain, with the timestamp riding in the edge's weight slot, so selecting a
// time window means walking the whole neighbourhood and testing every edge.
//
// Query file layout (little-endian, repeated): u64 vertex, u64 t_lo, u64 t_hi.
// Path comes from the GNOMON_QUERY_FILE environment variable.
//
// E33 additions (docs/E33_SPEC.md, phase q2): each query is timed and the
// loop runs under the phase budget (GNOMON_PHASE_BUDGET_SEC, default 1800 s;
// the first 20 queries are timed and the total projected). The original
// output lines are unchanged; "Window Median us", "Window P95 us" and
// "Window Done = k/N" are appended. Helpers come from e33_queries.h.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

template <class T>
void mem_window_query(gview_t<T>* snaph)
{
    const char* qpath = getenv("GNOMON_QUERY_FILE");
    if (!qpath) {
        cout << "GNOMON_QUERY_FILE not set" << endl;
        return;
    }
    FILE* qf = fopen(qpath, "rb");
    if (!qf) { perror(qpath); return; }

    struct Query { uint64_t v, lo, hi; };
    std::vector<Query> queries;
    Query q;
    while (fread(&q, sizeof(Query), 1, qf) == 1) queries.push_back(q);
    fclose(qf);
    cout << "window queries = " << queries.size() << endl;
    if (queries.empty()) return;

    // Edges still in the log rather than the adjacency. After a full archive
    // this is empty; fetched once rather than per query so an unarchived tail
    // costs a scan, not a scan per query.
    edgeT_t<T>* tail_edges = 0;
    index_t tail_marker = snaph->get_nonarchived_edges(tail_edges);

    uint64_t checksum = 0;
    index_t delivered = 0;
    index_t scanned = 0;

    std::vector<double> lat_us;
    double total = 0;
    size_t done = e33_run_budgeted(queries.size(), e33_budget_sec(), lat_us, &total,
        [&](size_t i) {
        const vid_t v = (vid_t)queries[i].v;
        const uint64_t lo = queries[i].lo, hi = queries[i].hi;

        delta_adjlist_t<T>* delta_adjlist = snaph->get_nebrs_archived_out(v);
        if (0 != delta_adjlist) {
            degree_t delta_degree = snaph->get_degree_out(v);
            while (delta_adjlist != 0 && delta_degree > 0) {
                T* local_adjlist = delta_adjlist->get_adjlist();
                degree_t local_degree = delta_adjlist->get_nebrcount();
                degree_t i_count = min(local_degree, delta_degree);
                for (degree_t k = 0; k < i_count; ++k) {
                    ++scanned;
                    uint64_t ts = (uint64_t)local_adjlist[k].second.value_sid;
                    if (ts >= lo && ts <= hi) {
                        checksum += ts ^ (uint64_t)get_sid(local_adjlist[k]);
                        ++delivered;
                    }
                }
                delta_adjlist = delta_adjlist->get_next();
                delta_degree -= local_degree;
            }
        }

        for (index_t j = 0; j < tail_marker; ++j) {
            if (TO_SID(tail_edges[j].src_id) != v) continue;
            ++scanned;
            uint64_t ts = (uint64_t)tail_edges[j].dst_id.second.value_sid;
            if (ts >= lo && ts <= hi) {
                checksum += ts ^ (uint64_t)get_sid(tail_edges[j].dst_id);
                ++delivered;
            }
        }
        });

    cout << "Window Query Time = " << total << endl;
    cout << "Window Delivered = " << delivered << endl;
    cout << "Window Scanned = " << scanned << endl;
    cout << "Window Checksum = " << checksum << endl;
    cout << "Window ns per delivered edge = "
         << (delivered ? total * 1e9 / delivered : 0.0) << endl;
    cout << "Window Median us = " << e33_percentile(lat_us, 0.5) << endl;
    cout << "Window P95 us = " << e33_percentile(lat_us, 0.95) << endl;
    cout << "Window Done = " << done << "/" << queries.size() << endl;
}
