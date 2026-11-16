#!/usr/bin/env python3
"""Apply a log-reduction policy to one UNICORN graph (base + stream edge lists)
and write the reduced graph in the same format, ready for UNICORN's analyzer.

Input/output format (crimson-unicorn parsers, ProvParser/provparser/up.py):
  base:   <src> <dst> <src_type>:<dst_type>:<edge_type>:<timestamp>
  stream: <src> <dst> <src_type>:<dst_type>:<edge_type>:<new_src>:<new_dst>:<timestamp>

Policies (same rules as benchmark/bench_e8_attack.cpp and bench_e14_tc.cpp):
  cpr    --param WINDOW_NS : causality-preserving reduction. Within a fixed time
         bucket (timestamp // WINDOW_NS) a repeated (src, dst) pair carries no new
         causal information, so only its first occurrence is kept.
  random --param DROP_PCT  : drop each edge independently with probability
         DROP_PCT/100 (seeded by --seed).
  none                     : identity (checks that re-splitting is lossless).

Base and stream are read as one time-ordered log, the policy is applied to it,
and the surviving log is re-parsed exactly as up.py would parse a reduced log:
node ids renumbered from 1 in order of first appearance, the first --base-size
surviving edges become the base graph, the rest the stream with new_src/new_dst
recomputed. So UNICORN sees what it would see if the reduction had been applied
to the audit log before its parser ran. With --policy none the output is
byte-identical to the input.

Prints: policy, param, edges in, edges out, reduction ratio (in / out).
"""
import argparse, random


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True)
    ap.add_argument('--stream', required=True)
    ap.add_argument('--out-base', required=True)
    ap.add_argument('--out-stream', required=True)
    ap.add_argument('--policy', choices=['none', 'cpr', 'random'], required=True)
    ap.add_argument('--param', type=float, default=0.0, help='cpr: window in ns; random: drop percentage')
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--base-size', type=int, default=10000)
    a = ap.parse_args()

    rng = random.Random(0xE8 ^ a.seed)
    win_ns = int(a.param) if a.policy == 'cpr' else 0
    p_drop = a.param / 100.0 if a.policy == 'random' else 0.0
    last_seen = {}
    ids = {}
    n_in = n_out = 0

    def records():
        with open(a.base) as f:
            for line in f:
                p = line.split()
                if len(p) >= 3:
                    st, dt, et, tt = p[2].split(':')
                    yield p[0], p[1], st, dt, et, tt
        with open(a.stream) as f:
            for line in f:
                p = line.split()
                if len(p) >= 3:
                    q = p[2].split(':')
                    yield p[0], p[1], q[0], q[1], q[2], q[5]

    with open(a.out_base, 'w') as bf, open(a.out_stream, 'w') as sf:
        for s, d, st, dt, et, tt in records():
            n_in += 1
            if win_ns > 0:
                w = int(tt) // win_ns
                key = (s, d)
                if last_seen.get(key) == w:
                    continue
                last_seen[key] = w
            elif p_drop > 0:
                if rng.random() < p_drop:
                    continue
            ns = nd = '0'
            if s not in ids:
                ids[s] = str(len(ids) + 1); ns = '1'
            if d not in ids:
                ids[d] = str(len(ids) + 1); nd = '1'
            if n_out < a.base_size:
                bf.write(f"{ids[s]} {ids[d]} {st}:{dt}:{et}:{tt}\n")
            else:
                sf.write(f"{ids[s]} {ids[d]} {st}:{dt}:{et}:{ns}:{nd}:{tt}\n")
            n_out += 1

    ratio = n_in / n_out if n_out else float('inf')
    print(f"policy={a.policy}\tparam={a.param:g}\tedges_in={n_in}\tedges_out={n_out}\tnodes_out={len(ids)}\treduction={ratio:.3f}x")


if __name__ == '__main__':
    main()
