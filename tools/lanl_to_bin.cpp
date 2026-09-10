// Convert the LANL "Comprehensive, Multi-Source Cyber-Security Events" dataset
// (Kent 2015) into Barry's .bin edge format with a unified node id space across
// all five source files, plus the red team ground truth resolved into that same
// id space.
//
// The graph models host-to-host interaction over time:
//   auth   src_computer -> dst_computer   (authentication == lateral movement)
//   flows  src_computer -> dst_computer   (network flow)
//   dns    src_computer -> resolved       (name lookup, implies a later connection)
//   proc   computer     -> process        (context only; adds no host-to-host edge)
//
// With --user_nodes, an authentication also emits src_computer -> user -> dst_computer,
// which models credential reuse as a propagation channel. Off by default because it
// substantially increases connectivity; it is a modelling knob worth ablating.
//
// Edges are streamed straight to disk, so memory stays proportional to the number of
// distinct entities (~93K) rather than the 1.65B events.
//
// Usage:
//   barry_lanl_to_bin --out data/bin/lanl \
//       --auth auth.txt.gz --flows flows.txt.gz --dns dns.txt.gz \
//       --proc proc.txt.gz --redteam redteam.txt.gz [--user_nodes] [--progress]

#include "barry/edge.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace barry;

// Edge class, carried in Edge::weight so downstream queries can filter by channel.
enum EdgeClass : int {
    EC_AUTH_SUCCESS = 1,
    EC_AUTH_FAILURE = 2,
    EC_FLOW         = 3,
    EC_DNS          = 4,
    EC_PROC_START   = 5,
    EC_PROC_END     = 6,
    EC_AUTH_USER    = 7,   // computer -> user / user -> computer (with --user_nodes)
};

enum Kind : uint8_t { K_COMPUTER = 0, K_USER, K_PROCESS };
static const char* kind_name(Kind k) {
    return k == K_COMPUTER ? "computer" : k == K_USER ? "user" : "process";
}

struct NodeTable {
    std::unordered_map<std::string, uint32_t> id_of;
    std::vector<Kind>        kind;
    std::vector<std::string> name;

    uint32_t intern(char prefix, const std::string& raw, Kind k) {
        std::string key;
        key.reserve(raw.size() + 2);
        key.push_back(prefix); key.push_back(':'); key += raw;
        auto it = id_of.find(key);
        if (it != id_of.end()) return it->second;
        uint32_t id = static_cast<uint32_t>(kind.size());
        id_of.emplace(std::move(key), id);
        kind.push_back(k);
        name.push_back(raw);
        return id;
    }
    bool lookup(char prefix, const std::string& raw, uint32_t* out) const {
        std::string key;
        key.reserve(raw.size() + 2);
        key.push_back(prefix); key.push_back(':'); key += raw;
        auto it = id_of.find(key);
        if (it == id_of.end()) return false;
        *out = it->second;
        return true;
    }
};

// Split a comma-delimited line in place. Returns the field count.
static size_t split_csv(char* line, char** fields, size_t max_fields) {
    size_t n = 0;
    char* p = line;
    fields[n++] = p;
    while (*p && n < max_fields) {
        if (*p == ',') { *p = '\0'; fields[n++] = p + 1; }
        ++p;
    }
    // strip trailing newline / CR from the last field
    char* last = fields[n - 1];
    size_t len = std::strlen(last);
    while (len && (last[len - 1] == '\n' || last[len - 1] == '\r')) last[--len] = '\0';
    return n;
}

struct Writer {
    FILE*  f = nullptr;
    std::vector<Edge> buf;
    size_t count = 0;
    uint64_t t_min = ULLONG_MAX, t_max = 0, out_of_order = 0, last_ts = 0;

    explicit Writer(const std::string& path) {
        f = std::fopen(path.c_str(), "wb");
        if (!f) { std::perror(path.c_str()); std::exit(1); }
        buf.reserve(1u << 20);
    }
    void put(uint32_t src, uint32_t dst, uint64_t ts, int cls) {
        buf.push_back(Edge{src, dst, ts, static_cast<float>(cls)});
        if (ts < t_min) t_min = ts;
        if (ts > t_max) t_max = ts;
        if (ts < last_ts) ++out_of_order;
        last_ts = ts;
        ++count;
        if (buf.size() == buf.capacity()) flush();
    }
    void flush() {
        if (buf.empty()) return;
        if (std::fwrite(buf.data(), sizeof(Edge), buf.size(), f) != buf.size()) {
            std::perror("fwrite"); std::exit(1);
        }
        buf.clear();
    }
    ~Writer() { flush(); if (f) std::fclose(f); }
};

// Open a possibly-gzipped file for reading as text.
static FILE* open_maybe_gz(const std::string& path) {
    std::string cmd = "zcat -f '" + path + "'";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) std::perror(cmd.c_str());
    return p;
}

int main(int argc, char** argv) {
    std::string out_prefix, p_auth, p_proc, p_flows, p_dns, p_redteam;
    bool user_nodes = false, progress = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--out"     && i + 1 < argc) out_prefix = argv[++i];
        else if (a == "--auth"    && i + 1 < argc) p_auth     = argv[++i];
        else if (a == "--proc"    && i + 1 < argc) p_proc     = argv[++i];
        else if (a == "--flows"   && i + 1 < argc) p_flows    = argv[++i];
        else if (a == "--dns"     && i + 1 < argc) p_dns      = argv[++i];
        else if (a == "--redteam" && i + 1 < argc) p_redteam  = argv[++i];
        else if (a == "--user_nodes") user_nodes = true;
        else if (a == "--progress")   progress   = true;
    }
    if (out_prefix.empty()) {
        std::fprintf(stderr, "usage: %s --out <prefix> [--auth f] [--proc f] "
                     "[--flows f] [--dns f] [--redteam f] [--user_nodes] [--progress]\n", argv[0]);
        return 1;
    }

    NodeTable nodes;
    Writer w(out_prefix + ".bin");

    // Red team events are a labelled subset of the auth stream. Load them first so
    // the auth pass can confirm each one resolves to a real edge.
    struct RT { uint64_t t; std::string user, src, dst; bool matched = false; };
    std::vector<RT> redteam;
    std::unordered_set<std::string> rt_key;
    if (!p_redteam.empty()) {
        FILE* f = open_maybe_gz(p_redteam);
        if (!f) return 1;
        char line[4096]; char* fld[8];
        while (std::fgets(line, sizeof(line), f)) {
            if (split_csv(line, fld, 8) < 4) continue;
            RT r;
            r.t    = std::strtoull(fld[0], nullptr, 10);
            r.user = fld[1]; r.src = fld[2]; r.dst = fld[3];
            rt_key.insert(std::string(fld[0]) + "|" + r.src + "|" + r.dst);
            redteam.push_back(std::move(r));
        }
        pclose(f);
        std::fprintf(stderr, "[lanl] loaded %zu red team events\n", redteam.size());
    }

    uint64_t n_auth = 0, n_proc = 0, n_flow = 0, n_dns = 0, n_rt_hit = 0;
    char line[8192]; char* fld[16];

    // ---- auth --------------------------------------------------------------
    // time,src_user@dom,dst_user@dom,src_computer,dst_computer,auth_type,logon_type,
    // auth_orientation,success/failure
    if (!p_auth.empty()) {
        FILE* f = open_maybe_gz(p_auth);
        if (!f) return 1;
        std::fprintf(stderr, "[lanl] reading auth ...\n");
        while (std::fgets(line, sizeof(line), f)) {
            char raw[8192];
            std::strncpy(raw, line, sizeof(raw) - 1); raw[sizeof(raw) - 1] = '\0';
            size_t nf = split_csv(line, fld, 16);
            if (nf < 9) continue;
            uint64_t t = std::strtoull(fld[0], nullptr, 10);
            const char* src_c = fld[3];
            const char* dst_c = fld[4];
            bool ok = (std::strncmp(fld[8], "Success", 7) == 0);
            if (src_c[0] == '?' || dst_c[0] == '?') continue;

            uint32_t s = nodes.intern('C', src_c, K_COMPUTER);
            uint32_t d = nodes.intern('C', dst_c, K_COMPUTER);
            w.put(s, d, t, ok ? EC_AUTH_SUCCESS : EC_AUTH_FAILURE);
            ++n_auth;

            if (user_nodes && fld[1][0] != '?') {
                uint32_t u = nodes.intern('U', fld[1], K_USER);
                w.put(s, u, t, EC_AUTH_USER);
                w.put(u, d, t, EC_AUTH_USER);
            }
            if (!rt_key.empty()) {
                std::string k = std::string(fld[0]) + "|" + src_c + "|" + dst_c;
                if (rt_key.count(k)) ++n_rt_hit;
            }
            if (progress && (n_auth % 100000000ULL == 0)) {
                std::fprintf(stderr, "  ... auth %lluM, %zu nodes\n",
                             (unsigned long long)(n_auth / 1000000), nodes.kind.size());
            }
        }
        pclose(f);
    }

    // ---- flows -------------------------------------------------------------
    // time,duration,src_computer,src_port,dst_computer,dst_port,protocol,packets,bytes
    if (!p_flows.empty()) {
        FILE* f = open_maybe_gz(p_flows);
        if (!f) return 1;
        std::fprintf(stderr, "[lanl] reading flows ...\n");
        while (std::fgets(line, sizeof(line), f)) {
            if (split_csv(line, fld, 16) < 9) continue;
            uint64_t t = std::strtoull(fld[0], nullptr, 10);
            if (fld[2][0] == '?' || fld[4][0] == '?') continue;
            uint32_t s = nodes.intern('C', fld[2], K_COMPUTER);
            uint32_t d = nodes.intern('C', fld[4], K_COMPUTER);
            w.put(s, d, t, EC_FLOW);
            ++n_flow;
        }
        pclose(f);
    }

    // ---- dns ---------------------------------------------------------------
    // time,src_computer,resolved_computer
    if (!p_dns.empty()) {
        FILE* f = open_maybe_gz(p_dns);
        if (!f) return 1;
        std::fprintf(stderr, "[lanl] reading dns ...\n");
        while (std::fgets(line, sizeof(line), f)) {
            if (split_csv(line, fld, 16) < 3) continue;
            uint64_t t = std::strtoull(fld[0], nullptr, 10);
            if (fld[1][0] == '?' || fld[2][0] == '?') continue;
            uint32_t s = nodes.intern('C', fld[1], K_COMPUTER);
            uint32_t d = nodes.intern('C', fld[2], K_COMPUTER);
            w.put(s, d, t, EC_DNS);
            ++n_dns;
        }
        pclose(f);
    }

    // ---- proc --------------------------------------------------------------
    // time,user@domain,computer,process_name,start/end
    if (!p_proc.empty()) {
        FILE* f = open_maybe_gz(p_proc);
        if (!f) return 1;
        std::fprintf(stderr, "[lanl] reading proc ...\n");
        while (std::fgets(line, sizeof(line), f)) {
            if (split_csv(line, fld, 16) < 5) continue;
            uint64_t t = std::strtoull(fld[0], nullptr, 10);
            if (fld[2][0] == '?' || fld[3][0] == '?') continue;
            bool start = (fld[4][0] == 'S');
            uint32_t c = nodes.intern('C', fld[2], K_COMPUTER);
            std::string pkey = std::string(fld[2]) + "/" + fld[3];
            uint32_t p = nodes.intern('P', pkey, K_PROCESS);
            if (start) w.put(c, p, t, EC_PROC_START);
            else       w.put(p, c, t, EC_PROC_END);
            ++n_proc;
        }
        pclose(f);
    }

    w.flush();

    // ---- resolve red team into the shared id space -------------------------
    size_t rt_resolved = 0;
    if (!redteam.empty()) {
        FILE* rf = std::fopen((out_prefix + ".redteam.tsv").c_str(), "w");
        if (!rf) { std::perror("redteam.tsv"); return 1; }
        std::fprintf(rf, "time\tuser\tsrc_id\tdst_id\tsrc_name\tdst_name\tresolved\n");
        for (const RT& r : redteam) {
            uint32_t s = UINT32_MAX, d = UINT32_MAX;
            bool oks = nodes.lookup('C', r.src, &s);
            bool okd = nodes.lookup('C', r.dst, &d);
            if (oks && okd) ++rt_resolved;
            std::fprintf(rf, "%llu\t%s\t%d\t%d\t%s\t%s\t%s\n",
                         (unsigned long long)r.t, r.user.c_str(),
                         oks ? (int)s : -1, okd ? (int)d : -1,
                         r.src.c_str(), r.dst.c_str(),
                         (oks && okd) ? "yes" : "no");
        }
        std::fclose(rf);
    }

    // ---- node dictionary ---------------------------------------------------
    {
        FILE* nf = std::fopen((out_prefix + ".nodes.tsv").c_str(), "w");
        if (!nf) { std::perror("nodes.tsv"); return 1; }
        std::fprintf(nf, "id\tkind\tname\n");
        for (size_t i = 0; i < nodes.kind.size(); ++i) {
            std::fprintf(nf, "%zu\t%s\t%s\n", i, kind_name(nodes.kind[i]), nodes.name[i].c_str());
        }
        std::fclose(nf);
    }

    size_t n_comp = 0, n_user = 0, n_procn = 0;
    for (Kind k : nodes.kind) {
        if (k == K_COMPUTER) ++n_comp; else if (k == K_USER) ++n_user; else ++n_procn;
    }

    {
        FILE* mf = std::fopen((out_prefix + ".meta.json").c_str(), "w");
        std::fprintf(mf,
            "{\n  \"edges\": %zu,\n  \"auth\": %llu,\n  \"flows\": %llu,\n  \"dns\": %llu,\n"
            "  \"proc\": %llu,\n  \"nodes\": %zu,\n  \"nodes_computer\": %zu,\n"
            "  \"nodes_user\": %zu,\n  \"nodes_process\": %zu,\n"
            "  \"t_min\": %llu,\n  \"t_max\": %llu,\n  \"t_span_sec\": %llu,\n"
            "  \"out_of_order\": %llu,\n  \"user_nodes\": %s,\n"
            "  \"redteam_events\": %zu,\n  \"redteam_resolved\": %zu,\n"
            "  \"redteam_matched_in_auth\": %llu\n}\n",
            w.count, (unsigned long long)n_auth, (unsigned long long)n_flow,
            (unsigned long long)n_dns, (unsigned long long)n_proc,
            nodes.kind.size(), n_comp, n_user, n_procn,
            (unsigned long long)w.t_min, (unsigned long long)w.t_max,
            (unsigned long long)(w.t_max - w.t_min),
            (unsigned long long)w.out_of_order, user_nodes ? "true" : "false",
            redteam.size(), rt_resolved, (unsigned long long)n_rt_hit);
        std::fclose(mf);
    }

    std::fprintf(stderr,
        "[lanl] %zu edges (auth=%llu flows=%llu dns=%llu proc=%llu)\n"
        "       %zu nodes (computer=%zu user=%zu process=%zu)\n"
        "       t=[%llu, %llu] span=%.1f days, out-of-order=%llu (%.3f%%)\n"
        "       redteam: %zu events, %zu resolved to node ids, %llu matched an auth edge\n",
        w.count, (unsigned long long)n_auth, (unsigned long long)n_flow,
        (unsigned long long)n_dns, (unsigned long long)n_proc,
        nodes.kind.size(), n_comp, n_user, n_procn,
        (unsigned long long)w.t_min, (unsigned long long)w.t_max,
        (w.t_max - w.t_min) / 86400.0, (unsigned long long)w.out_of_order,
        100.0 * w.out_of_order / (w.count ? w.count : 1),
        redteam.size(), rt_resolved, (unsigned long long)n_rt_hit);
    return 0;
}
