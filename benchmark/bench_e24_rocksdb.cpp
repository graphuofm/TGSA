// E24: RocksDB as the general-purpose KV baseline (TVA Fig. 7(g)(h) parity).
//
// The key schema is the one a KV store would actually be given for this
// workload, and the one TVA's own Full Mode uses for its RocksDB backend: a
// big-endian time-encoded key
//
//     key = ts(8B, BE) : src(4B, BE) : dst(4B, BE),   value = weight(4B)
//
// so the LSM's lexicographic order IS time order. That hands RocksDB its best
// case on the time axis: a window extract is one iterator seek plus a
// sequential scan, no filtering. What the schema cannot give it is structural
// awareness: the neighbors of one vertex inside a window are scattered through
// the whole window's key range, so a per-vertex expansion must scan the window
// and filter by src. Both faces are measured, and the extract result set is
// checksum-compared against TGSR's query_range on identical questions.
//
//   ingest         WriteBatch stream, WAL off (volatile, comparable to our
//                  no-log row) and WAL on (comparable to write_only/async).
//   extract        E22-style trailing-window scans across widths.
//   neighbors      E19-style per-vertex windowed expansion.
//
// Usage:
//   bench_e24_rocksdb --dataset <bin> [--limit N] [--batch N] [--samples N]
//                     [--windows 0.0001,0.001,0.01,0.1] --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <arpa/inet.h>
#include <random>
#include <string>
#include <vector>

using namespace barry;
using namespace barry::bench;

namespace {

inline uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }
inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

struct Key {
    uint64_t ts_be;
    uint32_t src_be, dst_be;
    // A KV Put overwrites, but the workload is a multiset: identical
    // (ts,src,dst) contacts are distinct events. A monotone sequence
    // suffix keeps every key unique so RocksDB answers the same question
    // the multiset stores answer; without it the wider windows failed
    // their checksum against TGSR by exactly the duplicate count.
    uint32_t seq_be;
} __attribute__((packed));
static_assert(sizeof(Key) == 20, "time-encoded key is 20 bytes");

inline Key make_key(const Edge& e, uint32_t seq) {
    return Key{be64(e.timestamp), be32(e.src), be32(e.dst), be32(seq)};
}
inline rocksdb::Slice key_slice(const Key& k) {
    return rocksdb::Slice(reinterpret_cast<const char*>(&k), sizeof(Key));
}

size_t rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    if (std::fscanf(f, "%lu %lu", &size, &resident) != 2) resident = 0;
    std::fclose(f);
    return static_cast<size_t>(resident) * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string out_csv = args.get("output", "");
    const std::string windows = args.get("windows", "0.0001,0.001,0.01,0.1");
    const std::string dbdir   = args.get("dbdir", "/tmp/e24_rocksdb");
    const size_t limit  = static_cast<size_t>(args.getl("limit", 0));
    const size_t batch  = static_cast<size_t>(args.getl("batch", 65536));
    const int  samples  = static_cast<int>(args.getl("samples", 2000));
    const int  threads  = static_cast<int>(args.getl("threads", 8));

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> [--limit N] [--batch N] "
                     "[--samples N] [--windows list] --output csv\n",
                     args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;

    CsvWriter w(out_csv,
        {"system","dataset","phase","param","edges","seconds","Meps_or_ms",
         "delivered","checksum","rss_MB"});

    // ------------------------------------------------ TGSR reference engine
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);
    Barry gn(cfg);
    {
        double t0 = now_sec();
        for (size_t off = 0; off < n; off += batch)
            gn.insert_batch(ef.edges + off, std::min(batch, n - off), threads);
        const double sec = now_sec() - t0;
        w.row(std::string("tgsr"), ds, std::string("ingest"), std::string("volatile"),
              (unsigned long long)n, sec, n / sec / 1e6, 0ull, 0ull,
              rss_bytes() / 1e6);
        std::fprintf(stderr, "[e24] tgsr    ingest %6.2f Meps\n", n / sec / 1e6);
        gn.finalize();
    }

    // ---------------------------------------------------------- RocksDB
    for (int wal = 0; wal <= 1; ++wal) {
        std::string dir = dbdir + (wal ? "_wal" : "_nowal");
        rocksdb::DestroyDB(dir, rocksdb::Options());
        rocksdb::Options opt;
        opt.create_if_missing = true;
        opt.compression = rocksdb::kNoCompression;
        opt.IncreaseParallelism(threads);
        opt.OptimizeLevelStyleCompaction();
        rocksdb::DB* db = nullptr;
        auto st = rocksdb::DB::Open(opt, dir, &db);
        if (!st.ok()) { std::fprintf(stderr, "open: %s\n", st.ToString().c_str()); return 1; }

        rocksdb::WriteOptions wo;
        wo.disableWAL = (wal == 0);
        double t0 = now_sec();
        for (size_t off = 0; off < n; off += batch) {
            const size_t m = std::min(batch, n - off);
            rocksdb::WriteBatch wb;
            for (size_t i = off; i < off + m; ++i) {
                const Edge& e = ef.edges[i];
                Key k = make_key(e, static_cast<uint32_t>(i));
                wb.Put(key_slice(k),
                       rocksdb::Slice(reinterpret_cast<const char*>(&e.weight), 4));
            }
            db->Write(wo, &wb);
        }
        // A fair ingest number includes making the data readable at speed:
        // flush the memtable so later reads are not a hidden compaction bill,
        // but time it inside the ingest figure, as our finalize() is in ours.
        db->Flush(rocksdb::FlushOptions());
        const double sec = now_sec() - t0;
        w.row(std::string("rocksdb"), ds, std::string("ingest"),
              std::string(wal ? "wal" : "volatile"),
              (unsigned long long)n, sec, n / sec / 1e6, 0ull, 0ull,
              rss_bytes() / 1e6);
        std::fprintf(stderr, "[e24] rocksdb ingest %6.2f Meps (wal=%d)\n",
                     n / sec / 1e6, wal);

        if (wal == 1) { delete db; rocksdb::DestroyDB(dir, rocksdb::Options()); break; }

        // ---- extract: trailing windows, checksum-compared against TGSR
        std::vector<double> wfracs;
        { std::string s2 = windows; size_t p;
          while ((p = s2.find(',')) != std::string::npos) {
              wfracs.push_back(std::strtod(s2.substr(0, p).c_str(), nullptr));
              s2 = s2.substr(p + 1); }
          if (!s2.empty()) wfracs.push_back(std::strtod(s2.c_str(), nullptr)); }

        for (double wf : wfracs) {
            const uint64_t width = std::max<uint64_t>(1, (uint64_t)(span * wf));
            const uint64_t t_lo = (meta.t_max > meta.t_min + width)
                                  ? meta.t_max - width : meta.t_min;
            // RocksDB: one seek, sequential scan to the end of the window.
            Key lo{be64(t_lo), 0, 0, 0};
            uint64_t rsum = 0; size_t rcnt = 0;
            double q0 = now_sec();
            std::unique_ptr<rocksdb::Iterator> it(
                db->NewIterator(rocksdb::ReadOptions()));
            for (it->Seek(key_slice(lo)); it->Valid(); it->Next()) {
                const Key* k = reinterpret_cast<const Key*>(it->key().data());
                const uint64_t ts = __builtin_bswap64(k->ts_be);
                if (ts > meta.t_max) break;
                rsum += ts ^ (uint64_t)__builtin_bswap32(k->dst_be);
                ++rcnt;
            }
            const double rms = (now_sec() - q0) * 1e3;
            // TGSR: same window through query_range.
            q0 = now_sec();
            std::vector<Edge> es = gn.query_range(t_lo, meta.t_max);
            const double gms = (now_sec() - q0) * 1e3;
            uint64_t gsum = 0;
            for (const Edge& e : es) gsum += e.timestamp ^ (uint64_t)e.dst;

            w.row(std::string("rocksdb"), ds, std::string("extract"), wf,
                  (unsigned long long)n, rms / 1e3, rms,
                  (unsigned long long)rcnt, (unsigned long long)rsum,
                  rss_bytes() / 1e6);
            w.row(std::string("tgsr"), ds, std::string("extract"), wf,
                  (unsigned long long)n, gms / 1e3, gms,
                  (unsigned long long)es.size(), (unsigned long long)gsum,
                  rss_bytes() / 1e6);
            std::fprintf(stderr,
                "[e24] extract w=%-8g rocksdb %9.1f ms  tgsr %9.1f ms  %s\n",
                wf, rms, gms,
                (rsum == gsum && rcnt == es.size()) ? "checksum MATCHES"
                                                    : "CHECKSUM DIFFERS");
        }

        // ---- neighbors: per-vertex windowed expansion, edge-uniform samples
        std::mt19937_64 rng(0xE24ull);
        const uint64_t width = std::max<uint64_t>(1, span / 100);
        std::vector<std::pair<uint32_t,uint64_t>> qs(samples);
        for (auto& q : qs) {
            const Edge& e = ef.edges[rng() % n];
            q = {e.src, e.timestamp};
        }
        uint64_t rsum = 0; size_t rcnt = 0;
        double q0 = now_sec();
        {
            std::unique_ptr<rocksdb::Iterator> it(
                db->NewIterator(rocksdb::ReadOptions()));
            for (auto& [v, lo] : qs) {
                Key k0{be64(lo), 0, 0, 0};
                for (it->Seek(key_slice(k0)); it->Valid(); it->Next()) {
                    const Key* k = reinterpret_cast<const Key*>(it->key().data());
                    const uint64_t ts = __builtin_bswap64(k->ts_be);
                    if (ts > lo + width) break;
                    if (__builtin_bswap32(k->src_be) == v) {
                        rsum += ts ^ (uint64_t)__builtin_bswap32(k->dst_be);
                        ++rcnt;
                    }
                }
            }
        }
        const double rms = (now_sec() - q0) * 1e3;
        const VertexIndex* vi = gn.vertex_index();
        std::vector<TemporalRef> sc;
        uint64_t gsum = 0; size_t gcnt = 0;
        q0 = now_sec();
        for (auto& [v, lo] : qs) {
            sc.clear();
            vi->neighbors_temporal_into(v, lo, lo + width, sc);
            for (const TemporalRef& r : sc) gsum += r.ts ^ (uint64_t)r.peer;
            gcnt += sc.size();
        }
        const double gms = (now_sec() - q0) * 1e3;
        w.row(std::string("rocksdb"), ds, std::string("neighbors"), samples,
              (unsigned long long)n, rms / 1e3, rms,
              (unsigned long long)rcnt, (unsigned long long)rsum, rss_bytes() / 1e6);
        w.row(std::string("tgsr"), ds, std::string("neighbors"), samples,
              (unsigned long long)n, gms / 1e3, gms,
              (unsigned long long)gcnt, (unsigned long long)gsum, rss_bytes() / 1e6);
        std::fprintf(stderr,
            "[e24] neighbors %d q: rocksdb %9.1f ms  tgsr %9.1f ms  %s\n",
            samples, rms, gms,
            (rsum == gsum && rcnt == gcnt) ? "checksum MATCHES" : "CHECKSUM DIFFERS");

        delete db;
        rocksdb::DestroyDB(dir, rocksdb::Options());
        // reopen loop continues to the WAL ingest measurement
        }
    return 0;
}
