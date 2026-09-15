// E18: Durability -- crash injection, recovery replay, and the price of the log.
//
// Theorem 6 says recovery IS re-ingestion: replaying the complete frames of the
// log through the normal insert path yields an engine query-equivalent to the
// pre-crash one restricted to acknowledged batches. A throughput number does not
// test that claim. Three modes do.
//
//   --mode crash   Fork a writer that ingests through the WAL, SIGKILL it at an
//                  arbitrary instant (no destructors, no flush, whatever the
//                  kernel left on disk is what we get), then replay and check
//                  three things: the recovered edges are exactly a prefix of the
//                  input, any trailing garbage was detected rather than ingested,
//                  and the recovered engine answers identically to a reference
//                  engine built from exactly those edges.
//
//   --mode curve   Replay wall time and rate against log size. This is the
//                  "recovery is re-ingestion" claim stated as a number, and it
//                  is what a full paper needs beside the WAL write throughput.
//
//   --mode cost    What durability costs on the ingest path: no log, async log
//                  (group commit), synchronous log, fsync per batch.
//
// Usage:
//   bench_e18_recovery --dataset <bin> --mode crash --trials 12 --output csv
//   bench_e18_recovery --dataset <bin> --mode curve --sizes 1,4,16,64 --output csv
//   bench_e18_recovery --dataset <bin> --mode cost  --limit 20000000 --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/wal.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

using namespace barry;
using namespace barry::bench;

namespace {

GraphConfig make_cfg(const GraphMeta& meta, size_t n, bool reverse = false) {
    GraphConfig cfg;
    cfg.t_min = meta.t_min;
    cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.build_reverse_index = reverse;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);
    return cfg;
}

// Canonical form of an engine's contents: every edge it holds, sorted. Two
// engines agree iff these agree, because every query in the interface is a
// predicate over the edge multiset (the argument Theorem 6 rests on).
std::vector<std::array<uint64_t, 3>> canonical(const Barry& b,
                                               uint64_t lo, uint64_t hi) {
    std::vector<Edge> all = b.query_range(lo, hi);
    std::vector<std::array<uint64_t, 3>> key;
    key.reserve(all.size());
    for (const Edge& e : all) key.push_back({e.timestamp, e.src, e.dst});
    std::sort(key.begin(), key.end());
    return key;
}

// The writer runs as a freshly exec'd process, not a fork of the harness. A
// fork inherits the parent's page tables, and once the parent has built a
// recovery engine the child pays copy-on-write faults that throttle it to a
// fraction of its real rate: the first trial then looks nothing like the rest.
// Exec'ing gives every trial the same clean address space a real collector has.
//
// Readiness is signalled on `ready_fd` after construction, so the kill delay is
// measured from the first ingested edge rather than from spawn; otherwise the
// axe lands during arena reservation and tests nothing.
[[noreturn]] int child_ingest(const std::string& in_path, size_t limit,
                              const std::string& wal_path,
                              size_t batch, int threads, int ready_fd) {
    EdgeFile ef;
    if (!ef.load(in_path)) _exit(3);
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    const GraphMeta meta = scan_meta(ef.edges, n);

    Barry b(make_cfg(meta, n));
    AsyncWal wal(wal_path, 4u << 20);
    { char c = 'R'; ssize_t r = ::write(ready_fd, &c, 1); (void)r; ::close(ready_fd); }
    for (size_t off = 0; off < n; off += batch) {
        size_t m = std::min(batch, n - off);
        wal.append(ef.edges + off, m);       // write-ahead
        b.insert_batch(ef.edges + off, m, threads);
    }
    wal.sync();
    _exit(0);                                 // no destructors, no atexit
}

}  // namespace

// ------------------------------------------------------------------- crash
static int run_crash(const EdgeFile& ef, const GraphMeta& /*whole*/,
                     const std::string& ds, int trials, size_t limit,
                     size_t batch, int threads, const std::string& out_csv,
                     const std::string& wal_dir, double kill_max_ms,
                     const std::string& self, const std::string& in_path) {
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    // Match the writer, which sizes its engine from the prefix it ingests.
    const GraphMeta meta = scan_meta(ef.edges, n);

    CsvWriter w(out_csv,
        {"system","dataset","trial","kill_after_ms","edges_offered","batch",
         "killed","edges_recovered","frames","torn_bytes","wal_bytes",
         "replay_sec","replay_Meps","prefix_exact","query_equivalent",
         "recovered_frac"});

    std::mt19937_64 rng(0xE18ull);
    int failures = 0;

    for (int t = 0; t < trials; ++t) {
        const std::string wal_path = wal_dir + "/e18_crash_" + std::to_string(t) + ".wal";
        ::unlink(wal_path.c_str());

        // Kill somewhere inside the ingest, sampled over a wide range so the
        // axe lands in different phases: mid-batch, mid-fsync, between frames.
        const double kill_ms = 20.0 + std::uniform_real_distribution<double>(0.0, 1.0)(rng) * kill_max_ms;

        int rfd[2];
        if (::pipe(rfd) != 0) { std::perror("pipe"); return 1; }
        pid_t pid = ::fork();
        if (pid < 0) { std::perror("fork"); return 1; }
        if (pid == 0) {
            ::close(rfd[0]);
            const std::string s_limit = std::to_string(n);
            const std::string s_batch = std::to_string(batch);
            const std::string s_thr   = std::to_string(threads);
            const std::string s_fd    = std::to_string(rfd[1]);
            const char* av[] = {self.c_str(), "--mode", "_writer",
                                "--dataset", in_path.c_str(),
                                "--limit", s_limit.c_str(),
                                "--batch", s_batch.c_str(),
                                "--threads", s_thr.c_str(),
                                "--wal_path", wal_path.c_str(),
                                "--ready_fd", s_fd.c_str(),
                                "--output", "/dev/null", nullptr};
            ::execv(self.c_str(), const_cast<char* const*>(av));
            std::perror("execv");
            _exit(4);
        }
        ::close(rfd[1]);
        { char c; ssize_t r = ::read(rfd[0], &c, 1); (void)r; ::close(rfd[0]); }

        ::usleep(static_cast<useconds_t>(kill_ms * 1000.0));
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        const bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;

        size_t wal_bytes = 0;
        { struct stat st{}; if (::stat(wal_path.c_str(), &st) == 0) wal_bytes = st.st_size; }

        // --- recovery: replay through the normal insert path
        Barry rec(make_cfg(meta, n));
        const double r0 = now_sec();
        Wal::ReplayStats st = Wal::replay(wal_path, rec, threads);
        const double replay_sec = now_sec() - r0;

        // --- check 1: recovered edges are exactly a prefix of the input
        bool prefix_exact = (st.edges <= n);
        if (prefix_exact && st.edges > 0) {
            std::vector<std::array<uint64_t, 3>> want;
            want.reserve(st.edges);
            for (size_t i = 0; i < st.edges; ++i)
                want.push_back({ef.edges[i].timestamp, ef.edges[i].src, ef.edges[i].dst});
            std::sort(want.begin(), want.end());
            prefix_exact = (canonical(rec, meta.t_min, meta.t_max) == want);
        }

        // --- check 2: query-equivalent to an engine built from those edges
        bool query_equiv = true;
        if (st.edges > 0) {
            Barry ref(make_cfg(meta, n));
            ref.insert_batch(ef.edges, st.edges, threads);
            query_equiv = (canonical(ref, meta.t_min, meta.t_max)
                        == canonical(rec, meta.t_min, meta.t_max));
            if (query_equiv) {
                // Beyond the edge multiset, agree on derived answers too.
                const uint64_t lo = meta.t_min, hi = meta.t_max;
                query_equiv = (ref.window_aggregate_sum(lo, hi) == rec.window_aggregate_sum(lo, hi));
                for (uint32_t s : {0u, 1u, 7u}) {
                    if (s > meta.v_max) break;
                    if (ref.bfs(s) != rec.bfs(s)) { query_equiv = false; break; }
                }
            }
        }

        if (!prefix_exact || !query_equiv || !killed) ++failures;

        w.row(std::string("gnomon"), ds, t, kill_ms,
              (unsigned long long)n, (unsigned long long)batch,
              (int)killed, (unsigned long long)st.edges,
              (unsigned long long)st.frames, (unsigned long long)st.torn_bytes,
              (unsigned long long)wal_bytes,
              replay_sec, st.edges / replay_sec / 1e6,
              (int)prefix_exact, (int)query_equiv,
              (double)st.edges / (double)n);

        std::fprintf(stderr,
            "[e18-crash] t=%2d kill@%6.1fms killed=%d recovered=%10llu/%llu (%5.1f%%) "
            "frames=%6llu torn=%6llu replay=%6.2fs %5.2f Meps prefix=%d equiv=%d\n",
            t, kill_ms, (int)killed, (unsigned long long)st.edges,
            (unsigned long long)n, 100.0 * st.edges / n,
            (unsigned long long)st.frames, (unsigned long long)st.torn_bytes,
            replay_sec, st.edges / replay_sec / 1e6,
            (int)prefix_exact, (int)query_equiv);

        ::unlink(wal_path.c_str());
    }
    std::fprintf(stderr, "[e18-crash] %d/%d trials clean\n", trials - failures, trials);
    return failures == 0 ? 0 : 2;
}

// ------------------------------------------------------------------- curve
static int run_curve(const EdgeFile& ef, const GraphMeta& /*whole*/,
                     const std::string& ds, const std::string& sizes_csv,
                     size_t batch, int threads, const std::string& out_csv,
                     const std::string& wal_dir) {
    CsvWriter w(out_csv,
        {"system","dataset","edges","wal_bytes","write_sec","write_Meps",
         "replay_sec","replay_Meps","replay_threads","bytes_per_edge"});

    std::vector<size_t> sizes;
    { std::string s = sizes_csv, tok; size_t p = 0;
      while ((p = s.find(',')) != std::string::npos) {
          sizes.push_back(std::strtoull(s.substr(0, p).c_str(), nullptr, 10) * 1000000ull);
          s = s.substr(p + 1);
      }
      if (!s.empty()) sizes.push_back(std::strtoull(s.c_str(), nullptr, 10) * 1000000ull);
    }

    const std::string wal_path = wal_dir + "/e18_curve.wal";
    for (size_t want : sizes) {
        const size_t n = std::min(want, ef.count);
        if (n == 0) continue;
        ::unlink(wal_path.c_str());

        // Configure from the prefix that is actually replayed, not from the
        // whole corpus. A prefix of a long corpus occupies a short time range,
        // so a mapper sized for the full span would put every edge of a small
        // prefix into a handful of windows and time chain-walking rather than
        // recovery. That is a property of the harness, not of replay.
        const GraphMeta meta = scan_meta(ef.edges, n);

        // Write the log exactly as the ingest path would, synchronously, so the
        // bytes on disk are the bytes a crash would have left.
        double w0 = now_sec();
        {
            Wal wal(wal_path, 4u << 20);
            for (size_t off = 0; off < n; off += batch)
                wal.append(ef.edges + off, std::min(batch, n - off));
            wal.sync();
        }
        const double write_sec = now_sec() - w0;

        size_t wal_bytes = 0;
        { struct stat st{}; if (::stat(wal_path.c_str(), &st) == 0) wal_bytes = st.st_size; }

        // Cold-ish read: the log was just written so it is in page cache. That
        // biases replay in our favour, so say so rather than hide it; the
        // interesting quantity here is the insert-path cost, not the disk.
        Barry rec(make_cfg(meta, n));
        const double r0 = now_sec();
        Wal::ReplayStats st = Wal::replay(wal_path, rec, threads);
        const double replay_sec = now_sec() - r0;

        w.row(std::string("gnomon"), ds, (unsigned long long)st.edges,
              (unsigned long long)wal_bytes, write_sec, n / write_sec / 1e6,
              replay_sec, st.edges / replay_sec / 1e6, threads,
              (double)wal_bytes / (double)st.edges);

        std::fprintf(stderr,
            "[e18-curve] %10llu edges  wal=%7.2f GB  write=%7.2fs (%5.2f Meps)  "
            "replay=%7.2fs (%5.2f Meps)\n",
            (unsigned long long)st.edges, wal_bytes / 1e9, write_sec,
            n / write_sec / 1e6, replay_sec, st.edges / replay_sec / 1e6);
    }
    ::unlink(wal_path.c_str());
    return 0;
}

// -------------------------------------------------------------------- torn
// A SIGKILL does not lose page cache: everything the process wrote is still in
// the file, so the crash mode almost never produces a torn frame. Power loss
// does, and the way to test that path is to inject it: truncate the log at an
// arbitrary byte (the tail that never reached the platter) or corrupt a byte
// inside a frame (a write that reached it wrong). The contract under test is
// not "nothing is lost" but "what is lost is detected": replay must stop at the
// last intact frame and must never ingest a partial or corrupt batch.
static int run_torn(const EdgeFile& ef, const GraphMeta& /*whole*/,
                    const std::string& ds, int trials, size_t limit,
                    size_t batch, int threads, const std::string& out_csv,
                    const std::string& wal_dir) {
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    const GraphMeta meta = scan_meta(ef.edges, n);   // the prefix, not the corpus

    const std::string base = wal_dir + "/e18_torn_base.wal";
    ::unlink(base.c_str());
    {
        Wal wal(base, 4u << 20);
        for (size_t off = 0; off < n; off += batch)
            wal.append(ef.edges + off, std::min(batch, n - off));
        wal.sync();
    }
    size_t base_bytes = 0;
    { struct stat st{}; if (::stat(base.c_str(), &st) != 0) { std::perror("stat"); return 1; }
      base_bytes = st.st_size; }

    // Frame table of the intact log, so we know the right answer independently
    // of the code under test.
    struct Frame { size_t end_byte; size_t cum_edges; };
    std::vector<Frame> frames;
    {
        size_t off = 0, cum = 0;
        for (size_t o = 0; o < n; o += batch) {
            size_t k = std::min(batch, n - o);
            off += 12 + k * sizeof(Edge);   // magic+count+crc, then payload
            cum += k;
            frames.push_back({off, cum});
        }
    }

    CsvWriter w(out_csv,
        {"system","dataset","trial","injection","cut_byte","log_bytes",
         "edges_expected","edges_recovered","frames","torn_bytes",
         "replay_sec","correct","ingested_garbage"});

    const std::string scratch = wal_dir + "/e18_torn.wal";
    std::mt19937_64 rng(0x707211ull);
    int failures = 0;

    for (int t = 0; t < trials; ++t) {
        const bool corrupt = (t % 3 == 2);       // two truncations, one bit flip
        size_t cut = std::uniform_int_distribution<size_t>(1, base_bytes)(rng);

        // fresh scratch copy
        {
            std::string cmd = "cp " + base + " " + scratch;
            if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "cp failed\n"); return 1; }
        }
        size_t expected = 0;
        if (corrupt) {
            // Flip one bit inside a frame payload; every frame at or after it
            // must be refused, so the expected recovery is the frames before.
            FILE* f = std::fopen(scratch.c_str(), "r+b");
            if (!f) { std::perror(scratch.c_str()); return 1; }
            std::fseek(f, static_cast<long>(cut - 1), SEEK_SET);
            int c = std::fgetc(f);
            std::fseek(f, static_cast<long>(cut - 1), SEEK_SET);
            std::fputc(c ^ 0x40, f);
            std::fclose(f);
            for (const Frame& fr : frames) {
                if (fr.end_byte <= cut - 1) expected = fr.cum_edges; else break;
            }
        } else {
            if (::truncate(scratch.c_str(), static_cast<off_t>(cut)) != 0) {
                std::perror("truncate"); return 1;
            }
            for (const Frame& fr : frames) {
                if (fr.end_byte <= cut) expected = fr.cum_edges; else break;
            }
        }

        Barry rec(make_cfg(meta, n));
        const double r0 = now_sec();
        Wal::ReplayStats st = Wal::replay(scratch, rec, threads);
        const double replay_sec = now_sec() - r0;

        const bool count_ok = (st.edges == expected);
        bool content_ok = true;
        if (st.edges > 0) {
            std::vector<std::array<uint64_t, 3>> want;
            want.reserve(st.edges);
            for (size_t i = 0; i < st.edges; ++i)
                want.push_back({ef.edges[i].timestamp, ef.edges[i].src, ef.edges[i].dst});
            std::sort(want.begin(), want.end());
            content_ok = (canonical(rec, meta.t_min, meta.t_max) == want);
        }
        // "Ingested garbage" is the failure that actually matters: more edges
        // than the intact prefix holds, or edges that are not the real ones.
        const bool garbage = (st.edges > expected) || !content_ok;
        const bool correct = count_ok && content_ok;
        if (!correct) ++failures;

        w.row(std::string("gnomon"), ds, t,
              std::string(corrupt ? "bitflip" : "truncate"),
              (unsigned long long)cut, (unsigned long long)base_bytes,
              (unsigned long long)expected, (unsigned long long)st.edges,
              (unsigned long long)st.frames, (unsigned long long)st.torn_bytes,
              replay_sec, (int)correct, (int)garbage);

        std::fprintf(stderr,
            "[e18-torn] t=%2d %-8s cut=%11llu/%llu expected=%9llu got=%9llu "
            "torn=%8llu correct=%d garbage=%d\n",
            t, corrupt ? "bitflip" : "truncate",
            (unsigned long long)cut, (unsigned long long)base_bytes,
            (unsigned long long)expected, (unsigned long long)st.edges,
            (unsigned long long)st.torn_bytes, (int)correct, (int)garbage);
    }
    ::unlink(scratch.c_str());
    ::unlink(base.c_str());
    std::fprintf(stderr, "[e18-torn] %d/%d trials correct\n", trials - failures, trials);
    return failures == 0 ? 0 : 2;
}

// -------------------------------------------------------------------- cost
static int run_cost(const EdgeFile& ef, const GraphMeta& /*whole*/,
                    const std::string& ds, size_t limit, size_t batch,
                    int threads, const std::string& out_csv,
                    const std::string& wal_dir) {
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    const GraphMeta meta = scan_meta(ef.edges, n);   // the prefix, not the corpus
    CsvWriter w(out_csv,
        {"system","dataset","durability","edges","batch","threads",
         "ingest_sec","ingest_Meps","wal_bytes"});

    const std::string wal_path = wal_dir + "/e18_cost.wal";

    struct Mode { const char* name; int kind; size_t sync_every; };
    // kind 0 = no log at all, 1 = async log (group commit), 2 = synchronous log,
    // 3 = fsync every batch, 4 = log written but never fsync'd.
    //
    // Mode 4 exists for one reason: it is the contract GraphOne's durable path
    // actually implements. Its log writer calls write() and leaves fsync()
    // commented out (src/sgraph.h), which is consistent with its own statement
    // that an unplanned shutdown may lose recent data and that recovery depends
    // on an upstream backup such as Kafka. Comparing our fsync'd numbers against
    // its unfsync'd ones would flatter us, so both are reported.
    const Mode modes[] = {
        {"none",        0, 0},
        {"write_only",  4, 0},
        {"async_64MB",  1, 64u << 20},
        {"async_4MB",   1, 4u << 20},
        {"sync_4MB",    2, 4u << 20},
        {"fsync_batch", 3, 1},
    };

    for (const Mode& m : modes) {
        ::unlink(wal_path.c_str());
        Barry b(make_cfg(meta, n));

        double sec_override = 0.0;
        double t0 = now_sec();
        if (m.kind == 0) {
            for (size_t off = 0; off < n; off += batch)
                b.insert_batch(ef.edges + off, std::min(batch, n - off), threads);
        } else if (m.kind == 1) {
            AsyncWal wal(wal_path, m.sync_every);
            for (size_t off = 0; off < n; off += batch) {
                size_t k = std::min(batch, n - off);
                wal.append(ef.edges + off, k);
                b.insert_batch(ef.edges + off, k, threads);
            }
            wal.sync();
        } else if (m.kind == 4) {
            // Never fsync: the bytes reach the page cache and no further. The
            // timer stops before the Wal is destroyed, since its destructor
            // fsyncs and that cost is not part of this contract.
            Wal wal(wal_path, SIZE_MAX);
            for (size_t off = 0; off < n; off += batch) {
                size_t k = std::min(batch, n - off);
                wal.append(ef.edges + off, k);
                b.insert_batch(ef.edges + off, k, threads);
            }
            sec_override = now_sec() - t0;
        } else {
            Wal wal(wal_path, m.kind == 3 ? 1 : m.sync_every);
            for (size_t off = 0; off < n; off += batch) {
                size_t k = std::min(batch, n - off);
                wal.append(ef.edges + off, k);
                b.insert_batch(ef.edges + off, k, threads);
            }
            wal.sync();
        }
        const double sec = (sec_override > 0) ? sec_override : now_sec() - t0;

        size_t wal_bytes = 0;
        { struct stat st{}; if (::stat(wal_path.c_str(), &st) == 0) wal_bytes = st.st_size; }

        w.row(std::string("gnomon"), ds, std::string(m.name),
              (unsigned long long)n, (unsigned long long)batch, threads,
              sec, n / sec / 1e6, (unsigned long long)wal_bytes);

        std::fprintf(stderr, "[e18-cost] %-12s %7.2fs  %6.2f Meps  wal=%7.2f GB\n",
                     m.name, sec, n / sec / 1e6, wal_bytes / 1e9);
    }
    ::unlink(wal_path.c_str());
    return 0;
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string mode    = args.get("mode", "crash");
    const std::string out_csv = args.get("output", "");
    const std::string wal_dir = args.get("wal_dir", "/tmp");
    const int    trials  = static_cast<int>(args.getl("trials", 10));
    const size_t limit   = static_cast<size_t>(args.getl("limit", 0));
    const size_t batch   = static_cast<size_t>(args.getl("batch", 1u << 20));
    const int    threads = static_cast<int>(args.getl("threads", 0));
    const std::string sizes = args.get("sizes", "1,4,16,64");

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr,
            "usage: %s --dataset <bin> --mode <crash|torn|curve|cost> [--trials N] "
            "[--limit N] [--batch N] [--threads T] [--sizes 1,4,16,64] "
            "[--wal_dir DIR] --output csv\n", args.prog().c_str());
        return 1;
    }

    if (mode == "_writer") {
        return child_ingest(in_path, limit, args.get("wal_path"), batch, threads,
                            static_cast<int>(args.getl("ready_fd", -1)));
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const GraphMeta meta = scan_meta(ef.edges, ef.count);
    const std::string ds = basename_noext(in_path);

    if (mode == "torn")  return run_torn(ef, meta, ds, trials, limit, batch, threads, out_csv, wal_dir);
    if (mode == "crash") return run_crash(ef, meta, ds, trials, limit, batch, threads, out_csv, wal_dir, args.getd("kill_max_ms", 900.0), argv[0], in_path);
    if (mode == "curve") return run_curve(ef, meta, ds, sizes, batch, threads, out_csv, wal_dir);
    if (mode == "cost")  return run_cost(ef, meta, ds, limit, batch, threads, out_csv, wal_dir);
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
}
