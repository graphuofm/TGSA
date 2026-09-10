// Shared utilities for benchmark binaries: timing, mmap-based loading, CSV
// emitting, simple CLI parsing. Header-only.

#pragma once

#include "barry/edge.hpp"
#include "barry/engine.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace barry { namespace bench {

inline double now_sec() {
    using clk = std::chrono::high_resolution_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

// ----------------------------------------------------------------------- CLI
class Args {
public:
    Args(int argc, char** argv) {
        if (argc > 0) prog_ = argv[0];
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
                std::string k = a.substr(2);
                std::string v;
                auto eq = k.find('=');
                if (eq != std::string::npos) { v = k.substr(eq + 1); k = k.substr(0, eq); }
                else if (i + 1 < argc) v = argv[++i];
                kv_[k] = v;
            }
        }
    }
    bool has(const std::string& k) const { return kv_.count(k) > 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = kv_.find(k); return it == kv_.end() ? def : it->second;
    }
    long getl(const std::string& k, long def = 0) const {
        if (!has(k)) return def; return std::strtol(get(k).c_str(), nullptr, 10);
    }
    double getd(const std::string& k, double def = 0.0) const {
        if (!has(k)) return def; return std::strtod(get(k).c_str(), nullptr);
    }
    const std::string& prog() const { return prog_; }
private:
    std::string prog_;
    std::map<std::string, std::string> kv_;
};

// ---------------------------------------------------------------------- mmap
struct EdgeFile {
    int fd = -1;
    void* addr = nullptr;
    size_t bytes = 0;
    const Edge* edges = nullptr;
    size_t count = 0;

    bool load(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { std::perror(path.c_str()); return false; }
        struct stat st{};
        if (::fstat(fd, &st) != 0) { std::perror("fstat"); return false; }
        bytes = static_cast<size_t>(st.st_size);
        if (bytes % sizeof(Edge) != 0) {
            std::fprintf(stderr, "%s: bad size %zu\n", path.c_str(), bytes); return false;
        }
        addr = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        if (addr == MAP_FAILED) { std::perror("mmap"); return false; }
        ::posix_madvise(addr, bytes, POSIX_MADV_SEQUENTIAL);
        edges = reinterpret_cast<const Edge*>(addr);
        count = bytes / sizeof(Edge);
        return true;
    }
    ~EdgeFile() {
        if (addr && addr != MAP_FAILED) ::munmap(addr, bytes);
        if (fd >= 0) ::close(fd);
    }
};

// Quick scan for (t_min, t_max, max_vertex_id).
struct GraphMeta {
    uint64_t t_min = ULLONG_MAX;
    uint64_t t_max = 0;
    uint32_t v_max = 0;
};
inline GraphMeta scan_meta(const Edge* e, size_t n) {
    GraphMeta m;
    if (n == 0) { m.t_min = 0; return m; }
    for (size_t i = 0; i < n; ++i) {
        if (e[i].timestamp < m.t_min) m.t_min = e[i].timestamp;
        if (e[i].timestamp > m.t_max) m.t_max = e[i].timestamp;
        if (e[i].src      > m.v_max) m.v_max = e[i].src;
        if (e[i].dst      > m.v_max) m.v_max = e[i].dst;
    }
    return m;
}

// ------------------------------------------------------------------- Stats
inline double percentile(std::vector<double>& xs, double q) {
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    double pos = q * (xs.size() - 1);
    size_t lo = static_cast<size_t>(pos);
    size_t hi = std::min(lo + 1, xs.size() - 1);
    double f  = pos - lo;
    return xs[lo] + (xs[hi] - xs[lo]) * f;
}
inline double median(std::vector<double> xs) { return percentile(xs, 0.5); }
inline double mean(const std::vector<double>& xs) {
    if (xs.empty()) return 0.0;
    double s = 0; for (double x : xs) s += x; return s / xs.size();
}

// ------------------------------------------------------------------- CSV
class CsvWriter {
public:
    CsvWriter(const std::string& path, const std::vector<std::string>& cols, bool append = true) {
        bool exists = false;
        if (append) {
            FILE* t = std::fopen(path.c_str(), "r");
            if (t) { std::fseek(t, 0, SEEK_END); exists = std::ftell(t) > 0; std::fclose(t); }
        }
        f_ = std::fopen(path.c_str(), append ? "a" : "w");
        if (!f_) { std::perror(path.c_str()); std::exit(1); }
        if (!append || !exists) {
            for (size_t i = 0; i < cols.size(); ++i) {
                std::fputs(cols[i].c_str(), f_);
                std::fputc(i + 1 == cols.size() ? '\n' : ',', f_);
            }
            std::fflush(f_);
        }
    }
    ~CsvWriter() { if (f_) std::fclose(f_); }
    template<typename... Args> void row(Args... args) {
        write_each(args...);
    }
private:
    template<typename T> void write_one(T x) {
        if (newline_pending_) { std::fputc(',', f_); }
        write_value(x);
        newline_pending_ = true;
    }
    void write_value(int          v) { std::fprintf(f_, "%d",  v); }
    void write_value(unsigned     v) { std::fprintf(f_, "%u",  v); }
    void write_value(long         v) { std::fprintf(f_, "%ld", v); }
    void write_value(unsigned long v){ std::fprintf(f_, "%lu", v); }
    void write_value(long long    v) { std::fprintf(f_, "%lld",v); }
    void write_value(unsigned long long v){ std::fprintf(f_, "%llu", v); }
    void write_value(double       v) { std::fprintf(f_, "%.9g", v); }
    void write_value(float        v) { std::fprintf(f_, "%.6g", v); }
    void write_value(const char*  v) { std::fprintf(f_, "%s",  v); }
    void write_value(const std::string& v) { std::fprintf(f_, "%s", v.c_str()); }
    template<typename A> void write_each(A a) {
        write_one(a);
        std::fputc('\n', f_); std::fflush(f_); newline_pending_ = false;
    }
    template<typename A, typename... Rest> void write_each(A a, Rest... rest) {
        write_one(a); write_each(rest...);
    }
    FILE* f_ = nullptr;
    bool  newline_pending_ = false;
};

// Strip directory + extension from a path -> "sx-superuser".
inline std::string basename_noext(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

}}  // namespace barry::bench
