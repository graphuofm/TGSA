#!/usr/bin/env python3
"""Turn the ProvParser-format edge list from unicorn_cdm_to_edgelist.py into
UNICORN graphs: one graph per CDM JSON file (UNICORN's own graph unit for DARPA
CADETS; its published CADETS graph statistics equal these per-file graphs
edge for edge), each split into a base graph (first --base-size edges) and a
stream, exactly as crimson-unicorn parsers/cdm/ProvParser/provparser/up.py
(-m -S base-size) does: node ids renumbered per graph from 1 in order of first
appearance; stream edges carry new_src/new_dst flags.

A graph is labelled attack iff its time span contains an attack hit (an event on a
NetFlow whose remote address is a ground-truth attack IP; attack_hits.NN.tsv);
otherwise benign. Output names: base/base-cadets-e3-<label>-NN.txt, stream/...,
and graphs.tsv with per-graph metadata.

usage: unicorn_make_graphs.py --edges-dir D --out-dir D [--base-size 10000]
"""
import argparse, glob, os, datetime


def day(ts):
    return datetime.datetime.utcfromtimestamp(ts / 1e9).strftime('%Y-%m-%d %H:%M')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--edges-dir', required=True)
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--base-size', type=int, default=10000)
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out_dir, 'base'), exist_ok=True)
    os.makedirs(os.path.join(a.out_dir, 'stream'), exist_ok=True)
    index = open(os.path.join(a.out_dir, 'graphs.tsv'), 'w')
    index.write('graph\tsource_file\tlabel\tedges\tnodes\tt_min_utc\tt_max_utc\tattack_hits\tattacks\n')
    for fn in sorted(glob.glob(os.path.join(a.edges_dir, 'edges.*.tsv'))):
        k = os.path.basename(fn).split('.')[1]
        hits = {}
        hf = os.path.join(a.edges_dir, f'attack_hits.{k}.tsv')
        if os.path.exists(hf):
            for line in open(hf):
                for x in line.split('\t')[1].split(':')[0].split(','):
                    hits[x] = hits.get(x, 0) + 1
        label = 'attack' if hits else 'benign'
        name = f'cadets-e3-{label}-{k}'
        ids = {}
        n = 0; tmin = None; tmax = None
        with open(fn) as f, open(os.path.join(a.out_dir, 'base', f'base-{name}.txt'), 'w') as bf, \
             open(os.path.join(a.out_dir, 'stream', f'stream-{name}.txt'), 'w') as sf:
            for line in f:
                s, d, attr = line.rstrip('\n').split('\t')
                st, dt, et, tt = attr.split(':')
                t = int(tt)
                tmin = t if tmin is None or t < tmin else tmin
                tmax = t if tmax is None or t > tmax else tmax
                ns = nd = '0'
                if s not in ids:
                    ids[s] = str(len(ids) + 1); ns = '1'
                if d not in ids:
                    ids[d] = str(len(ids) + 1); nd = '1'
                if n < a.base_size:
                    bf.write(f"{ids[s]} {ids[d]} {st}:{dt}:{et}:{tt}\n")
                else:
                    sf.write(f"{ids[s]} {ids[d]} {st}:{dt}:{et}:{ns}:{nd}:{tt}\n")
                n += 1
        atk = ','.join(f'{x}({hits[x]})' for x in sorted(hits))
        index.write(f"{name}\t{os.path.basename(fn)}\t{label}\t{n}\t{len(ids)}\t{day(tmin)}\t{day(tmax)}\t{sum(hits.values())}\t{atk}\n")
        index.flush()
        print(name, n, len(ids), flush=True)
    index.close()


if __name__ == '__main__':
    main()
