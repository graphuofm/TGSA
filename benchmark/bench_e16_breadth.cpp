// E16: breadth across corpora. Separates the two numbers a streaming engine
// owes: raw ingestion (what the stream sees) and time-to-query-ready
// (ingestion plus the one optional compaction pass). Also reports peak RSS
// and a time-respecting traversal median on the same load.
#include "common.hpp"
#include "barry/engine.hpp"
#include <random>
#include <sys/resource.h>

using namespace barry;
using namespace barry::bench;
static size_t rss_mb(){ struct rusage u; getrusage(RUSAGE_SELF,&u); return u.ru_maxrss/1024; }

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in = args.get("dataset"), out = args.get("output");
    int threads = (int)args.getl("threads", 8), samples = (int)args.getl("num_samples", 200);
    double wf = args.getd("window_frac", 0.01);
    if (in.empty() || out.empty()) { std::fprintf(stderr, "--dataset --output required\n"); return 1; }

    EdgeFile ef; if (!ef.load(in)) return 1;
    GraphMeta m = scan_meta(ef.edges, ef.count);
    const uint32_t V = m.v_max + 1;
    const uint64_t span = std::max<uint64_t>(1, m.t_max - m.t_min);
    // Corpora carry timestamps in seconds (SNAP, LANL) or nanoseconds (CDM).
    // Detect the unit from the span so reported durations are comparable: no
    // recorded trace is 10^5 days long, so a span that large must be sub-second
    // units.
    const double secs_per_tick = (span > 100ULL*365*86400) ? 1e-9 : 1.0;

    GraphConfig cfg;
    cfg.t_min = m.t_min; cfg.t_max = m.t_max; cfg.num_vertices = V;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL<<30, ef.count*sizeof(Edge)*4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL<<28, ef.count*sizeof(EdgeRef)*4);
    Barry b(cfg);

    double t0 = now_sec();
    b.insert_batch(ef.edges, ef.count, threads);
    const double ingest_s = now_sec() - t0;      // what the stream sees
    t0 = now_sec();
    b.finalize();
    const double compact_s = now_sec() - t0;     // optional, off the ingest path

    std::mt19937_64 rng(0x16);
    const VertexIndex* vi = b.vertex_index();
    const uint64_t win = std::max<uint64_t>(1, (uint64_t)(span*wf));
    std::vector<double> lat; lat.reserve(samples);
    double tot_r = 0;
    for (int i = 0; i < samples; ++i) {
        uint32_t seed = 0;
        for (int k = 0; k < 64; ++k) {
            uint32_t v = std::uniform_int_distribution<uint32_t>(0, V-1)(rng);
            if (vi->out_degree(v) > 0) { seed = v; break; }
        }
        uint64_t lo = m.t_min + std::uniform_int_distribution<uint64_t>(0, span-1)(rng);
        double a = now_sec();
        auto r = b.forward_taint(seed, lo, std::min(m.t_max, lo+win));
        lat.push_back((now_sec()-a)*1e6); tot_r += r.size();
    }
    CsvWriter w(out, {"dataset","edges","vertices","span_days","ingest_sec","ingest_Meps",
                      "compact_sec","ready_sec","ready_Meps","traverse_med_us","traverse_p95_us",
                      "avg_result","rss_MB","bytes_per_edge","threads"});
    w.row(basename_noext(in), (unsigned long long)ef.count, (unsigned long long)V,
          span*secs_per_tick/86400.0, ingest_s, ef.count/ingest_s/1e6, compact_s,
          ingest_s+compact_s, ef.count/(ingest_s+compact_s)/1e6,
          median(lat), percentile(lat,0.95), tot_r/samples,
          (unsigned long long)rss_mb(), rss_mb()*1048576.0/ef.count, threads);
    std::fprintf(stderr, "[e16] %-22s %11llu edges  ingest %6.2f Meps  +compact %5.2fs  "
                 "ready %5.2f Meps  trav %6.1f us  %5.1f B/edge\n",
                 basename_noext(in).c_str(), (unsigned long long)ef.count,
                 ef.count/ingest_s/1e6, compact_s, ef.count/(ingest_s+compact_s)/1e6,
                 median(lat), rss_mb()*1048576.0/ef.count);
    return 0;
}
