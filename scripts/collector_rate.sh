#!/bin/bash
# Security-relevant event rate of one host under routine load, without root.
#
# auditd and eBPF need root, which this machine does not grant, so the proxy is
# the system-call stream of a representative workload counted by strace -c -f.
# The count is exact (strace sees every call); the RATE is the count divided by
# the UNTRACED wall time of the same workload, because strace itself slows the
# workload several-fold. Only calls a provenance collector records are counted:
# process lifecycle, file open/close/read/write/unlink/rename, network
# connect/accept/send/recv, and exec.
#
# Workloads (each run twice: untraced for wall time, traced for counts):
#   build    cmake build of this repo's benchmark targets, 8 jobs (compiler-heavy)
#   python   the figure regeneration scripts (interpreter-heavy, many small files)
#   io       tar + gzip of the results directory (file-heavy)
# Output: results/e36_collector/rate.csv  (workload, wall_s, calls, calls_per_s)
set -u
cd "$(dirname "$0")/.."
OUT=results/e36_collector; mkdir -p "$OUT"
CSV=$OUT/rate.csv; echo "workload,wall_s,provenance_calls,calls_per_s,all_calls" > "$CSV"
TRACE_CALLS="execve,execveat,fork,vfork,clone,clone3,exit_group,openat,open,close,read,write,pread64,pwrite64,readv,writev,unlink,unlinkat,rename,renameat,renameat2,connect,accept,accept4,sendto,recvfrom,sendmsg,recvmsg,socket,bind,mmap,truncate,ftruncate,link,symlink,chmod,fchmod,chown,fchown"

run_workload() {  # $1 name, rest = command
    local name=$1; shift
    rm -rf build_rate; mkdir -p build_rate
    local t0=$(date +%s.%N); "$@" > /dev/null 2>&1; local t1=$(date +%s.%N)
    local wall=$(echo "$t1 - $t0" | bc -l)
    rm -rf build_rate; mkdir -p build_rate
    strace -f -c -e trace="$TRACE_CALLS" -o "$OUT/strace_$name.txt" "$@" > /dev/null 2>&1
    local calls=$(awk '/^total|^100\.00/{print $4}' "$OUT/strace_$name.txt" | tail -1)
    strace -f -c -o "$OUT/strace_all_$name.txt" "$@" > /dev/null 2>&1
    local allc=$(awk '/^total|^100\.00/{print $4}' "$OUT/strace_all_$name.txt" | tail -1)
    local rate=$(echo "$calls / $wall" | bc -l)
    printf "%s,%.2f,%s,%.0f,%s\n" "$name" "$wall" "$calls" "$rate" "$allc" >> "$CSV"
    echo "[$name] wall=${wall}s provenance_calls=$calls rate=$(printf %.0f $rate)/s all=$allc"
}

run_workload build  sh -c 'cmake -S . -B build_rate -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build_rate --target barry bench_e1_ingest bench_e2_query -j8'
run_workload python sh -c 'cd paper_SIGMOD/figures && for f in fig_ingest fig_queries fig_analytics fig_recovery fig_analysis; do python3 $f.py; done'
run_workload io     sh -c 'tar czf build_rate/results.tgz results && tar xzf build_rate/results.tgz -C build_rate'
rm -rf build_rate
echo "done: $(date)"
