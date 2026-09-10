// Convert DARPA Transparent Computing CDM records (line-delimited JSON) into
// Barry's flat .bin edge format plus a node dictionary.
//
// The CDM stream mixes node records (Subject / FileObject / NetFlowObject / ...)
// with Event records. We assign a dense uint32 id to every UUID on first sight,
// keep a human-readable label per node, and turn each information-flow Event
// into one Barry Edge{src, dst, timestampNanos, event_type_code}.
//
// Edge DIRECTION encodes information flow, which is what makes forward taint and
// backward causality tracing meaningful. See kFlow[] below for the full policy.
//
// Usage:
//   tar -xzOf ta1-cadets-e3-official.json.tar.gz | \
//     barry_tc_cdm_to_bin --out data/bin/tc-cadets-e3 [--sort] [--progress]
//
//   barry_tc_cdm_to_bin --in file.json --out prefix
//
// Outputs:
//   <prefix>.bin           packed Edge records (Barry's standard format)
//   <prefix>.nodes.tsv     id \t kind \t label
//   <prefix>.meta.json     counts, time range, event-type histogram

#include "barry/edge.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace barry;

// --------------------------------------------------------------- flow policy
enum Flow : uint8_t {
    FLOW_NONE = 0,   // no information flow -- drop the event
    FLOW_O2S,        // object  -> subject   (process reads / ingests)
    FLOW_S2O,        // subject -> object    (process writes / emits)
    FLOW_S2S,        // subject -> subject   (process creates process)
};

struct FlowRule { const char* name; Flow flow; };

// Information-flow direction per CDM EventType. Anything absent is dropped.
// Grounded in the standard provenance reading: an edge points the way data moves.
static const FlowRule kFlow[] = {
    // ---- data enters the process -------------------------------------------
    {"EVENT_READ",                 FLOW_O2S},
    {"EVENT_RECVFROM",             FLOW_O2S},
    {"EVENT_RECVMSG",              FLOW_O2S},
    {"EVENT_READ_SOCKET_PARAMS",   FLOW_O2S},
    {"EVENT_LOADLIBRARY",          FLOW_O2S},
    {"EVENT_MMAP",                 FLOW_O2S},
    {"EVENT_ACCEPT",               FLOW_O2S},
    {"EVENT_EXECUTE",              FLOW_O2S},   // the binary's content enters the process
    // ---- data leaves the process -------------------------------------------
    {"EVENT_WRITE",                FLOW_S2O},
    {"EVENT_SENDTO",               FLOW_S2O},
    {"EVENT_SENDMSG",              FLOW_S2O},
    {"EVENT_WRITE_SOCKET_PARAMS",  FLOW_S2O},
    {"EVENT_CREATE_OBJECT",        FLOW_S2O},
    {"EVENT_TRUNCATE",             FLOW_S2O},
    {"EVENT_UNLINK",               FLOW_S2O},
    {"EVENT_RENAME",               FLOW_S2O},
    {"EVENT_LINK",                 FLOW_S2O},
    {"EVENT_UPDATE",               FLOW_S2O},
    {"EVENT_MODIFY_FILE_ATTRIBUTES", FLOW_S2O},
    {"EVENT_CONNECT",              FLOW_S2O},
    {"EVENT_BIND",                 FLOW_S2O},
    {"EVENT_MPROTECT",             FLOW_S2O},
    {"EVENT_MODIFY_PROCESS",       FLOW_S2O},
    {"EVENT_SIGNAL",               FLOW_S2O},
    {"EVENT_SERVICEINSTALL",       FLOW_S2O},
    {"EVENT_STARTSERVICE",         FLOW_S2O},
    {"EVENT_LOGCLEAR",             FLOW_S2O},
    // ---- process creation ---------------------------------------------------
    {"EVENT_FORK",                 FLOW_S2S},
    {"EVENT_CLONE",                FLOW_S2S},
    {"EVENT_CREATE_THREAD",        FLOW_S2S},
    {"EVENT_CHANGE_PRINCIPAL",     FLOW_S2S},
};
static constexpr size_t kNumFlowRules = sizeof(kFlow) / sizeof(kFlow[0]);

// Returns FLOW_NONE for unlisted types; `code` receives a stable small id so the
// Edge weight can carry the event type for later filtering.
static Flow flow_of(const std::string& type, uint32_t* code) {
    for (size_t i = 0; i < kNumFlowRules; ++i) {
        if (type == kFlow[i].name) { *code = static_cast<uint32_t>(i) + 1; return kFlow[i].flow; }
    }
    *code = 0;
    return FLOW_NONE;
}

// ---------------------------------------------------------- tiny JSON probing
// The CDM JSON is machine-generated and line-delimited, so targeted substring
// scans are both correct and far faster than a general-purpose parser.

// Position just past `"key":` on this line, or npos.
static size_t after_key(const std::string& s, const char* key, size_t keylen) {
    size_t p = 0;
    while ((p = s.find(key, p)) != std::string::npos) {
        // Require the match to be a full quoted key followed by a colon.
        size_t q = p + keylen;
        if (q < s.size() && s[q] == ':') return q + 1;
        p = q;
    }
    return std::string::npos;
}

// Read a double-quoted string starting at or after `p`.
static bool read_quoted(const std::string& s, size_t p, std::string* out) {
    size_t a = s.find('"', p);
    if (a == std::string::npos) return false;
    size_t b = s.find('"', a + 1);
    if (b == std::string::npos) return false;
    out->assign(s, a + 1, b - a - 1);
    return true;
}

// Handles both a bare "key":"VALUE" and an Avro union "key":{"...":"VALUE"}.
// Returns false when the field is absent or explicitly null.
static bool get_str(const std::string& s, const char* quoted_key, std::string* out) {
    size_t keylen = std::strlen(quoted_key);
    size_t p = after_key(s, quoted_key, keylen);
    if (p == std::string::npos) return false;
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    if (p >= s.size()) return false;
    if (s.compare(p, 4, "null") == 0) return false;
    if (s[p] == '{') {
        // Union wrapper: skip the inner type name and its colon.
        size_t colon = s.find(':', p);
        if (colon == std::string::npos) return false;
        return read_quoted(s, colon + 1, out);
    }
    return read_quoted(s, p, out);
}

// Handles "key":12345 and "key":{"long":12345}.
static bool get_u64(const std::string& s, const char* quoted_key, uint64_t* out) {
    size_t keylen = std::strlen(quoted_key);
    size_t p = after_key(s, quoted_key, keylen);
    if (p == std::string::npos) return false;
    while (p < s.size() && (std::isspace(static_cast<unsigned char>(s[p])) ||
                            s[p] == '{' || s[p] == '"' || s[p] == ':')) {
        if (s[p] == '{' || s[p] == '"') {
            // Step over a union wrapper like {"long":
            size_t colon = s.find(':', p);
            if (colon == std::string::npos) return false;
            p = colon + 1;
            continue;
        }
        ++p;
    }
    if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
    *out = std::strtoull(s.c_str() + p, nullptr, 10);
    return true;
}

// ------------------------------------------------------------- node registry
enum Kind : uint8_t { K_UNKNOWN = 0, K_PROCESS, K_FILE, K_NETFLOW, K_OTHER };
static const char* kind_name(Kind k) {
    switch (k) {
        case K_PROCESS: return "process";
        case K_FILE:    return "file";
        case K_NETFLOW: return "netflow";
        case K_OTHER:   return "other";
        default:        return "unknown";
    }
}

struct NodeTable {
    std::unordered_map<std::string, uint32_t> id_of;
    std::vector<Kind>        kind;
    std::vector<std::string> label;

    uint32_t intern(const std::string& uuid) {
        auto it = id_of.find(uuid);
        if (it != id_of.end()) return it->second;
        uint32_t id = static_cast<uint32_t>(kind.size());
        id_of.emplace(uuid, id);
        kind.push_back(K_UNKNOWN);
        label.emplace_back();
        return id;
    }
    void describe(const std::string& uuid, Kind k, const std::string& lbl) {
        uint32_t id = intern(uuid);
        if (kind[id] == K_UNKNOWN) kind[id] = k;
        if (label[id].empty() && !lbl.empty()) label[id] = lbl;
    }
};

int main(int argc, char** argv) {
    std::string in_path, out_prefix;
    bool do_sort = false, progress = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--in"   && i + 1 < argc) in_path    = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_prefix = argv[++i];
        else if (a == "--sort")     do_sort  = true;
        else if (a == "--progress") progress = true;
    }
    if (out_prefix.empty()) {
        std::fprintf(stderr,
            "usage: %s --out <prefix> [--in <file.json>] [--sort] [--progress]\n"
            "       reads stdin when --in is omitted\n", argv[0]);
        return 1;
    }

    std::istream* in = &std::cin;
    std::ifstream fin;
    if (!in_path.empty()) {
        fin.open(in_path);
        if (!fin) { std::perror(in_path.c_str()); return 1; }
        in = &fin;
    }

    NodeTable nodes;
    std::vector<Edge> edges;
    edges.reserve(1u << 22);

    uint64_t lines = 0, n_event = 0, n_dropped = 0, n_no_endpoint = 0;
    uint64_t t_min = ULLONG_MAX, t_max = 0;
    uint64_t out_of_order = 0, last_ts = 0;
    std::unordered_map<std::string, uint64_t> type_hist;

    std::string line, uuid, subj, pobj, ptype, path, cmd;
    while (std::getline(*in, line)) {
        ++lines;
        if (progress && (lines % 2000000 == 0)) {
            std::fprintf(stderr, "  ... %llu lines, %llu edges, %zu nodes\n",
                         (unsigned long long)lines, (unsigned long long)edges.size(),
                         nodes.kind.size());
        }
        if (line.size() < 16) continue;

        // ---- node records -------------------------------------------------
        if (line.find(".Subject\":") != std::string::npos) {
            if (get_str(line, "\"uuid\"", &uuid)) {
                cmd.clear();
                get_str(line, "\"cmdLine\"", &cmd);
                nodes.describe(uuid, K_PROCESS, cmd);
            }
            continue;
        }
        if (line.find(".FileObject\":") != std::string::npos) {
            if (get_str(line, "\"uuid\"", &uuid)) {
                path.clear();
                get_str(line, "\"predicateObjectPath\"", &path);
                nodes.describe(uuid, K_FILE, path);
            }
            continue;
        }
        if (line.find(".NetFlowObject\":") != std::string::npos) {
            if (get_str(line, "\"uuid\"", &uuid)) {
                std::string addr; uint64_t port = 0;
                get_str(line, "\"remoteAddress\"", &addr);
                get_u64(line, "\"remotePort\"", &port);
                nodes.describe(uuid, K_NETFLOW,
                               addr.empty() ? std::string() : addr + ":" + std::to_string(port));
            }
            continue;
        }
        if (line.find(".UnnamedPipeObject\":") != std::string::npos ||
            line.find(".MemoryObject\":")      != std::string::npos ||
            line.find(".SrcSinkObject\":")     != std::string::npos ||
            line.find(".RegistryKeyObject\":") != std::string::npos) {
            if (get_str(line, "\"uuid\"", &uuid)) nodes.describe(uuid, K_OTHER, "");
            continue;
        }

        // ---- event records --------------------------------------------------
        if (line.find(".Event\":") == std::string::npos) continue;
        ++n_event;

        if (!get_str(line, "\"type\"", &ptype)) { ++n_dropped; continue; }
        uint32_t code = 0;
        Flow f = flow_of(ptype, &code);
        type_hist[ptype]++;
        if (f == FLOW_NONE) { ++n_dropped; continue; }

        uint64_t ts = 0;
        if (!get_u64(line, "\"timestampNanos\"", &ts) || ts == 0) { ++n_dropped; continue; }

        if (!get_str(line, "\"subject\"", &subj))         { ++n_no_endpoint; continue; }
        if (!get_str(line, "\"predicateObject\"", &pobj)) { ++n_no_endpoint; continue; }

        // Attach a path label opportunistically -- many FileObjects only ever
        // get named on the events that touch them.
        if (get_str(line, "\"predicateObjectPath\"", &path) && !path.empty()) {
            nodes.describe(pobj, K_FILE, path);
        }

        uint32_t s_id = nodes.intern(subj);
        uint32_t o_id = nodes.intern(pobj);
        uint32_t src, dst;
        if (f == FLOW_O2S)      { src = o_id; dst = s_id; }
        else                    { src = s_id; dst = o_id; }   // S2O and S2S

        edges.push_back(Edge{src, dst, ts, static_cast<float>(code)});
        if (ts < t_min) t_min = ts;
        if (ts > t_max) t_max = ts;
        if (ts < last_ts) ++out_of_order;
        last_ts = ts;
    }

    if (edges.empty()) {
        std::fprintf(stderr, "no edges produced from %llu lines\n", (unsigned long long)lines);
        return 2;
    }
    if (do_sort) {
        std::stable_sort(edges.begin(), edges.end(),
                         [](const Edge& a, const Edge& b) { return a.timestamp < b.timestamp; });
    }

    // ---- write .bin --------------------------------------------------------
    std::string bin_path = out_prefix + ".bin";
    FILE* bf = std::fopen(bin_path.c_str(), "wb");
    if (!bf) { std::perror(bin_path.c_str()); return 1; }
    if (std::fwrite(edges.data(), sizeof(Edge), edges.size(), bf) != edges.size()) {
        std::perror("fwrite"); std::fclose(bf); return 1;
    }
    std::fclose(bf);

    // ---- write node dictionary --------------------------------------------
    std::string nodes_path = out_prefix + ".nodes.tsv";
    FILE* nf = std::fopen(nodes_path.c_str(), "w");
    if (!nf) { std::perror(nodes_path.c_str()); return 1; }
    std::fprintf(nf, "id\tkind\tlabel\n");
    for (size_t i = 0; i < nodes.kind.size(); ++i) {
        std::string lbl = nodes.label[i];
        for (char& c : lbl) if (c == '\t' || c == '\n') c = ' ';
        std::fprintf(nf, "%zu\t%s\t%s\n", i, kind_name(nodes.kind[i]), lbl.c_str());
    }
    std::fclose(nf);

    // ---- write meta --------------------------------------------------------
    size_t n_proc = 0, n_file = 0, n_net = 0, n_unk = 0;
    for (Kind k : nodes.kind) {
        if (k == K_PROCESS) ++n_proc;
        else if (k == K_FILE) ++n_file;
        else if (k == K_NETFLOW) ++n_net;
        else if (k == K_UNKNOWN) ++n_unk;
    }
    std::vector<std::pair<uint64_t, std::string>> hist;
    for (auto& kv : type_hist) hist.emplace_back(kv.second, kv.first);
    std::sort(hist.rbegin(), hist.rend());

    std::string meta_path = out_prefix + ".meta.json";
    FILE* mf = std::fopen(meta_path.c_str(), "w");
    if (!mf) { std::perror(meta_path.c_str()); return 1; }
    std::fprintf(mf,
        "{\n  \"lines\": %llu,\n  \"events\": %llu,\n  \"edges\": %zu,\n"
        "  \"dropped_no_flow\": %llu,\n  \"dropped_no_endpoint\": %llu,\n"
        "  \"nodes\": %zu,\n  \"nodes_process\": %zu,\n  \"nodes_file\": %zu,\n"
        "  \"nodes_netflow\": %zu,\n  \"nodes_unknown\": %zu,\n"
        "  \"t_min\": %llu,\n  \"t_max\": %llu,\n  \"t_span_ns\": %llu,\n"
        "  \"out_of_order_events\": %llu,\n  \"sorted\": %s,\n"
        "  \"event_type_histogram\": {\n",
        (unsigned long long)lines, (unsigned long long)n_event, edges.size(),
        (unsigned long long)n_dropped, (unsigned long long)n_no_endpoint,
        nodes.kind.size(), n_proc, n_file, n_net, n_unk,
        (unsigned long long)t_min, (unsigned long long)t_max,
        (unsigned long long)(t_max - t_min),
        (unsigned long long)out_of_order, do_sort ? "true" : "false");
    for (size_t i = 0; i < hist.size(); ++i) {
        std::fprintf(mf, "    \"%s\": %llu%s\n", hist[i].second.c_str(),
                     (unsigned long long)hist[i].first,
                     i + 1 == hist.size() ? "" : ",");
    }
    std::fprintf(mf, "  }\n}\n");
    std::fclose(mf);

    std::fprintf(stderr,
        "[tc_cdm] %llu lines -> %zu edges over %zu nodes "
        "(proc=%zu file=%zu net=%zu unk=%zu)\n"
        "         span=%.2f days, out-of-order=%llu (%.2f%%), dropped: no-flow=%llu no-endpoint=%llu\n"
        "         wrote %s, %s, %s\n",
        (unsigned long long)lines, edges.size(), nodes.kind.size(),
        n_proc, n_file, n_net, n_unk,
        (t_max - t_min) / 86400e9, (unsigned long long)out_of_order,
        100.0 * out_of_order / edges.size(),
        (unsigned long long)n_dropped, (unsigned long long)n_no_endpoint,
        bin_path.c_str(), nodes_path.c_str(), meta_path.c_str());
    return 0;
}
