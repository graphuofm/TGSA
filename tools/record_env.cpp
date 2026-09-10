// barry_record_env: print a small JSON describing the runtime environment.
// Useful header for every results CSV directory.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>

static std::string read_all(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static std::string first_line_with(const std::string& body, const char* needle) {
    size_t p = body.find(needle);
    if (p == std::string::npos) return {};
    size_t e = body.find('\n', p);
    return body.substr(p, (e == std::string::npos ? body.size() : e) - p);
}

static std::string after_colon(const std::string& s) {
    size_t p = s.find(':');
    if (p == std::string::npos) return {};
    size_t i = p + 1;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

int main() {
    struct utsname un{}; uname(&un);
    std::string cpu = read_all("/proc/cpuinfo");
    std::string mem = read_all("/proc/meminfo");
    std::string model = after_colon(first_line_with(cpu, "model name"));
    std::string total = after_colon(first_line_with(mem, "MemTotal"));
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    char now_iso[64];
    time_t t = time(nullptr); struct tm tm; gmtime_r(&t, &tm);
    strftime(now_iso, sizeof now_iso, "%Y-%m-%dT%H:%M:%SZ", &tm);

    auto esc = [](const std::string& s) {
        std::string out; out.reserve(s.size());
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n' || c == '\r' || c == '\t') out += ' ';
            else out += c;
        }
        return out;
    };

    std::printf("{\n");
    std::printf("  \"timestamp_utc\": \"%s\",\n", now_iso);
    std::printf("  \"hostname\":      \"%s\",\n", un.nodename);
    std::printf("  \"kernel\":        \"%s %s\",\n", un.sysname, un.release);
    std::printf("  \"cpu_model\":     \"%s\",\n", esc(model).c_str());
    std::printf("  \"online_cpus\":   %ld,\n", ncpu);
    std::printf("  \"mem_total\":     \"%s\",\n", esc(total).c_str());
#ifdef __VERSION__
    std::printf("  \"compiler\":      \"%s\",\n", esc(__VERSION__).c_str());
#endif
    std::printf("  \"barry_build\":   \"%s\"\n",
#ifdef NDEBUG
                "release"
#else
                "debug"
#endif
    );
    std::printf("}\n");
    return 0;
}
