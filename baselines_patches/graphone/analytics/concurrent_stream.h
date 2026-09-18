#pragma once

// E26: concurrent throughput protocol on GraphOne. Added for the cross-system
// comparison, not part of upstream GraphOne; the identical protocol runs on
// TGSR and LiveGraph in bench_e26_concurrent.cpp.
//
//   preload  first GNOMON_PRELOAD edges through batch_edge from W threads,
//            then archive is waited to catch up (create_marker + spin), so
//            the settled history is fully adjacency-resident before timing.
//   stream   remaining edges through batch_edge from W threads pulling batch
//            offsets off a shared atomic, GraphOne's lock-free multi-writer
//            edge-log path at full speed.
//   readers  R threads answer windowed-neighbour queries continuously until
//            the writers finish, against static views over the archived
//            adjacency; views are refreshed every GNOMON_VIEW_REFRESH
//            queries (default 256) so snapshot acquisition stays in the
//            measured loop. Consistent (non-STALE) views are used: with
//            STALE_MASK, views taken while the archiver extends hot delta
//            chains returned wrong answers for settled windows on
//            tc-cadets (150 of 3,276 queries at 8 writers / 4 readers),
//            so the stale path is not safe under concurrent archiving and
//            the consistent path is what a correct reader must pay for.
//
// Being generous where it matters: the readers skip the non-archived tail
// scan entirely (settled windows cannot gain edges from it), which spares
// GraphOne the per-query cost its own window kernel pays for the tail.
//
// Env: GNOMON_QUERY_FILE  (u64 v, lo, hi triples)
//      GNOMON_EXPECT_FILE (u64 per query: count + sum of ts^dst)
//      GNOMON_PRELOAD, GNOMON_WRITERS, GNOMON_READERS,
//      GNOMON_BATCH (default 65536), GNOMON_VIEW_REFRESH (default 256)
//
// Output lines (parsed by scripts/run_e26.py):
//   E26 Preload Time / E26 Stream Time / E26 Stream Meps / E26 Queries /
//   E26 QPS / E26 Mismatches / E26 Delivered / E26 Archive Catchup

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

template <class T>
static uint64_t e26_answer(snap_t<T>* snaph, uint64_t v, uint64_t lo,
                           uint64_t hi, uint64_t* got)
{
    uint64_t checksum = 0, delivered = 0;
    delta_adjlist_t<T>* delta_adjlist = snaph->get_nebrs_archived_out((vid_t)v);
    if (0 != delta_adjlist) {
        degree_t delta_degree = snaph->get_degree_out((vid_t)v);
        while (delta_adjlist != 0 && delta_degree > 0) {
            T* local_adjlist = delta_adjlist->get_adjlist();
            degree_t local_degree = delta_adjlist->get_nebrcount();
            degree_t i_count = min(local_degree, delta_degree);
            for (degree_t k = 0; k < i_count; ++k) {
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
    *got = delivered;
    return checksum + delivered;
}

template <class T>
void mem_concurrent_stream(pgraph_t<T>* pgraph, edgeT_t<T>* edges, index_t total)
{
    const char* qpath = getenv("GNOMON_QUERY_FILE");
    const char* xpath = getenv("GNOMON_EXPECT_FILE");
    const char* pl    = getenv("GNOMON_PRELOAD");
    if (!qpath || !xpath || !pl) {
        cout << "E26 env not set (GNOMON_QUERY_FILE/GNOMON_EXPECT_FILE/GNOMON_PRELOAD)" << endl;
        return;
    }
    const index_t preload = (index_t)strtoull(pl, 0, 10);
    const int W = getenv("GNOMON_WRITERS") ? atoi(getenv("GNOMON_WRITERS")) : 8;
    const int R = getenv("GNOMON_READERS") ? atoi(getenv("GNOMON_READERS")) : 4;
    const index_t batch = getenv("GNOMON_BATCH") ?
        (index_t)strtoull(getenv("GNOMON_BATCH"), 0, 10) : 65536;
    const int refresh = getenv("GNOMON_VIEW_REFRESH") ?
        atoi(getenv("GNOMON_VIEW_REFRESH")) : 256;

    struct Query { uint64_t v, lo, hi; };
    std::vector<Query> qs;
    {
        FILE* f = fopen(qpath, "rb");
        if (!f) { perror(qpath); return; }
        Query q;
        while (fread(&q, sizeof q, 1, f) == 1) qs.push_back(q);
        fclose(f);
    }
    std::vector<uint64_t> expect;
    {
        FILE* f = fopen(xpath, "rb");
        if (!f) { perror(xpath); return; }
        uint64_t h;
        while (fread(&h, sizeof h, 1, f) == 1) expect.push_back(h);
        fclose(f);
    }
    if (qs.empty() || qs.size() != expect.size()) {
        cout << "E26 bad protocol files" << endl;
        return;
    }
    const int samples = (int)qs.size();
    cout << "E26 Queries Loaded = " << samples << endl;

    // ---- preload, W threads, then wait until the archive holds all of it
    double p0 = mywtime();
    {
        std::atomic<index_t> next{0};
        std::vector<std::thread> th;
        for (int t = 0; t < W; ++t) {
            th.emplace_back([&] {
                for (;;) {
                    index_t off = next.fetch_add(batch);
                    if (off >= preload) break;
                    index_t m = min(batch, preload - off);
                    for (index_t i = off; i < off + m; ++i)
                        pgraph->batch_edge(edges[i]);
                }
            });
        }
        for (auto& t : th) t.join();
        pgraph->create_marker(preload);
        pgraph->waitfor_archive(preload);
    }
    double preload_sec = mywtime() - p0;
    cout << "E26 Preload Time = " << preload_sec << endl;

    // ---- concurrent phase
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> mismatches{0}, delivered{0}, queries{0};
    std::vector<std::thread> rth;
    for (int r = 0; r < R; ++r) {
        rth.emplace_back([&, r] {
            uint64_t seed = 0xBEEFull + r;
            uint64_t local_q = 0, local_del = 0, local_mis = 0;
            snap_t<T>* snaph = 0;
            int since = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (!snaph || ++since >= refresh) {
                    if (snaph) delete_static_view(snaph);
                    snaph = create_static_view(pgraph, V_CENTRIC);
                    since = 0;
                }
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                const int i = (int)((seed >> 33) % samples);
                uint64_t got = 0;
                const uint64_t h = e26_answer<T>(snaph, qs[i].v, qs[i].lo,
                                                 qs[i].hi, &got);
                ++local_q;
                local_del += got;
                if (h != expect[i]) ++local_mis;
            }
            if (snaph) delete_static_view(snaph);
            queries.fetch_add(local_q);
            delivered.fetch_add(local_del);
            mismatches.fetch_add(local_mis);
        });
    }

    std::atomic<index_t> next{preload};
    double w0 = mywtime();
    std::vector<std::thread> wth;
    for (int t = 0; t < W; ++t) {
        wth.emplace_back([&] {
            for (;;) {
                index_t off = next.fetch_add(batch);
                if (off >= total) break;
                index_t m = min(batch, total - off);
                for (index_t i = off; i < off + m; ++i)
                    pgraph->batch_edge(edges[i]);
            }
        });
    }
    for (auto& t : wth) t.join();
    double stream_sec = mywtime() - w0;
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : rth) t.join();

    // ---- asynchronous archive catch-up, GraphOne's deferred cost
    double a0 = mywtime();
    pgraph->create_marker(total);
    pgraph->waitfor_archive(total);
    double archive_sec = mywtime() - a0;

    const index_t streamed = total - preload;
    cout << "E26 Stream Time = " << stream_sec << endl;
    cout << "E26 Stream Meps = " << streamed / stream_sec / 1e6 << endl;
    cout << "E26 Queries = " << queries.load() << endl;
    cout << "E26 QPS = " << queries.load() / stream_sec << endl;
    cout << "E26 Mismatches = " << mismatches.load() << endl;
    cout << "E26 Delivered = " << delivered.load() << endl;
    cout << "E26 Archive Catchup = " << archive_sec << endl;
}
