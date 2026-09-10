#!/bin/sh
# Provenance-relevant syscall rate across workload classes (paper §2.1).
# Counts via strace -f -c; RATE uses the UNTRACED wall clock (strace slows the
# workload heavily, so counts and time come from separate runs).
SC=${1:-/tmp/evrate.$$}; mkdir -p "$SC"
PROV='openat|open|read|write|pread64|pwrite64|execve|clone|fork|vfork|connect|accept|sendto|recvfrom|unlink|unlinkat|rename|renameat|mmap|close|stat|fstat|newfstatat|lstat'
run() { # $1=name $2=command
  T0=$(date +%s.%N); sh -c "$2" >/dev/null 2>&1; T1=$(date +%s.%N)
  strace -f -c -o "$SC/$1.st" sh -c "$2" >/dev/null 2>&1
  python3 - "$SC/$1.st" "$T0" "$T1" "$1" <<'PY'
import sys, re
prov = set("openat open read write pread64 pwrite64 execve clone fork vfork connect accept sendto recvfrom unlink unlinkat rename renameat mmap close stat fstat newfstatat lstat".split())
tot = pn = 0
for line in open(sys.argv[1]):
    m = re.match(r'\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)(?:\s+\d+)?\s+(\S+)\s*$', line)
    if m:
        c, n = int(m.group(1)), m.group(2); tot += c
        if n in prov: pn += c
w = float(sys.argv[3]) - float(sys.argv[2])
print(f"{sys.argv[4]:<16} wall={w:7.2f}s  prov_calls={pn:>10,}  rate={pn/max(w,1e-9):>12,.0f} /s")
PY
}
run idle          'sleep 3'
run build_j8      'rm -rf '"$SC"'/bt; cmake -S /home/jding/Barry -B '"$SC"'/bt -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1; cmake --build '"$SC"'/bt -j8 --target barry'
run io_archive    'tar -czf '"$SC"'/a.tgz /home/jding/Barry/src /home/jding/Barry/include /home/jding/Barry/benchmark'
run analytics     'python3 -c "
import csv,glob
n=0
for f in glob.glob(\"/home/jding/Barry/results/*/*.csv\"):
    for r in csv.reader(open(f)): n+=len(r)
print(n)"'
run git_ops       'git status >/dev/null && git log --stat >/dev/null && git diff HEAD~1 >/dev/null'
