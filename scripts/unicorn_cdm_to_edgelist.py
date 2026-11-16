#!/usr/bin/env python3
"""Convert DARPA TC E3 CDM18 JSON (CADETS) into UNICORN's ProvParser edge-list
format, reproducing the rules of crimson-unicorn/parsers/cdm/ProvParser
(partool/jparse.py: parsedp, gendp, processevent, valgendp) in Python 3 with an
in-memory node table instead of RocksDB.

Pass 1: every non-Event record with a uuid -> node type string (and, for
        NetFlowObject, the remote address).
Pass 2: every Event -> one directed edge (two for the bidirectional event types),
        in file order, written as
            <src_node_idx> \t <dst_node_idx> \t <src_type>:<dst_type>:<event_type>:<timestampNanos>
        Node indices are global integers assigned in order of first appearance
        in pass 1 (file order).  Events whose subject/object uuid is unknown are
        skipped, exactly as ProvParser does.

A side file records every event that touches a NetFlow whose remote address is
in the attack IP list (from data/tc_cadets_groundtruth.tsv), so attack periods
can be located afterwards.

usage: unicorn_cdm_to_edgelist.py --json-dir DIR --out-dir DIR --attack-ips FILE [--procs N]
"""
import argparse, json, os, sys, time
from multiprocessing import Pool

EV = 'com.bbn.tc.schema.avro.cdm18.Event'
UUIDKEY = 'com.bbn.tc.schema.avro.cdm18.UUID'
T_SOCK = 'com.bbn.tc.schema.avro.cdm18.NetFlowObject'
T_PIPE = 'com.bbn.tc.schema.avro.cdm18.UnnamedPipeObject'
T_MEM = 'com.bbn.tc.schema.avro.cdm18.MemoryObject'
T_HOST = 'com.bbn.tc.schema.avro.cdm18.Host'

SUB_OBJ = {'EVENT_CLOSE', 'EVENT_CREATE_OBJECT', 'EVENT_FORK', 'EVENT_OPEN', 'EVENT_LSEEK',
           'EVENT_CHANGE_PRINCIPAL', 'EVENT_LOGIN', 'EVENT_MODIFY_PROCESS', 'EVENT_EXECUTE',
           'EVENT_CONNECT', 'EVENT_SENDTO', 'EVENT_WRITE', 'EVENT_MODIFY_FILE_ATTRIBUTES',
           'EVENT_TRUNCATE', 'EVENT_UNLINK', 'EVENT_SIGNAL', 'EVENT_MPROTECT', 'EVENT_SENDMSG',
           'EVENT_BIND', 'EVENT_WRITE_SOCKET_PARAMS', 'EVENT_CREATE_THREAD', 'EVENT_LOGOUT',
           'EVENT_CLONE', 'EVENT_UNIT', 'EVENT_LOGCLEAR', 'EVENT_MOUNT', 'EVENT_SERVICEINSTALL',
           'EVENT_STARTSERVICE', 'EVENT_UMOUNT'}
BIDIR = {'EVENT_FCNTL', 'EVENT_MMAP', 'EVENT_OTHER'}
OBJ_SUB = {'EVENT_ACCEPT', 'EVENT_READ', 'EVENT_RECVFROM', 'EVENT_RECVMSG',
           'EVENT_CHECK_FILE_ATTRIBUTES', 'EVENT_READ_SOCKET_PARAMS', 'EVENT_LOADLIBRARY', 'EVENT_WAIT'}
OBJ_OBJ = {'EVENT_ADD_OBJECT_ATTRIBUTE', 'EVENT_LINK', 'EVENT_RENAME', 'EVENT_FLOWS_TO',
           'EVENT_UPDATE', 'EVENT_SHM', 'EVENT_CORRELATION'}


def sort_names(names):
    # ProvParser's sortfilenames: by numeric suffix, then name. We instead want
    # the natural temporal order: file, file.1, file.2, ... per tarball, tarballs
    # in the order official, official-1, official-2 (this is the time order of
    # the E3 CADETS release).
    def key(n):
        stem, _, suf = n.partition('.json')
        idx = int(suf[1:]) if suf.startswith('.') and suf[1:].isdigit() else 0
        rank = {'ta1-cadets-e3-official': 0, 'ta1-cadets-e3-official-1': 1, 'ta1-cadets-e3-official-2': 2}.get(stem, 9)
        return (rank, idx, n)
    return sorted(names, key=key)


def uuid_of(x):
    if x is None:
        return None
    return x.get(UUIDKEY)


def pass1(path):
    nodes = []  # (uuid, type, remote_addr or None) in order of appearance
    nrec = 0
    with open(path, 'rb') as f:
        for line in f:
            nrec += 1
            if b'cdm18.Event' in line[:60]:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            datum = rec['datum']
            rtype, rval = next(iter(datum.items()))
            if rtype == EV:
                continue
            try:
                u = rval['uuid']
                if rtype == T_SOCK:
                    val = 'NET_FLOW_OBJECT'
                    addr = rval.get('remoteAddress')
                elif rtype == T_PIPE:
                    val = 'UNNAMED_PIPE_OBJECT'; addr = None
                elif rtype == T_MEM:
                    val = 'MEMORY_OBJECT'; addr = None
                elif rtype == T_HOST:
                    val = rval['hostType']; addr = None
                else:
                    val = rval['type']; addr = None
                nodes.append((u, val, addr))
            except Exception:
                continue
    return path, nrec, nodes


G_NODES = None
G_ATTACK = None


def pass2(args):
    path, out_path, hit_path = args
    nodes = G_NODES
    attack = G_ATTACK
    n_ev = n_edges = n_skip = 0
    with open(path, 'rb') as f, open(out_path, 'w') as out, open(hit_path, 'w') as hits:
        for line in f:
            if b'cdm18.Event' not in line[:60]:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            rval = rec['datum'].get(EV)
            if rval is None:
                continue
            n_ev += 1
            et = rval.get('type')
            ts = rval.get('timestampNanos')
            if et is None or ts is None:
                n_skip += 1; continue
            bidir = False
            if et in SUB_OBJ:
                s, d = uuid_of(rval.get('subject')), uuid_of(rval.get('predicateObject'))
            elif et in BIDIR:
                s, d = uuid_of(rval.get('subject')), uuid_of(rval.get('predicateObject')); bidir = True
            elif et in OBJ_SUB:
                d, s = uuid_of(rval.get('subject')), uuid_of(rval.get('predicateObject'))
            elif et in OBJ_OBJ:
                s, d = uuid_of(rval.get('predicateObject')), uuid_of(rval.get('predicateObject2'))
            else:
                n_skip += 1; continue
            if s is None or d is None:
                n_skip += 1; continue
            sv = nodes.get(s); dv = nodes.get(d)
            if sv is None or dv is None:
                n_skip += 1; continue
            si, st = sv; di, dt = dv
            out.write(f"{si}\t{di}\t{st}:{dt}:{et}:{ts}\n")
            n_edges += 1
            if bidir:
                out.write(f"{di}\t{si}\t{dt}:{st}:{et}:{ts}\n")
                n_edges += 1
            a = attack.get(si) or attack.get(di)
            if a is not None:
                hits.write(f"{ts}\t{a}\t{et}\n")
    return path, n_ev, n_edges, n_skip


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json-dir', required=True)
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--attack-ips', required=True, help='TSV with columns attack,kind,match,...; netflow rows give IPs')
    ap.add_argument('--procs', type=int, default=10)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)

    ips = {}
    with open(a.attack_ips) as f:
        for line in f:
            if line.startswith('#') or line.startswith('attack'):
                continue
            p = line.rstrip('\n').split('\t')
            if len(p) >= 3 and p[1] == 'netflow':
                ips.setdefault(p[2], []).append(p[0])
    files = sort_names([n for n in os.listdir(a.json_dir) if '.json' in n])
    print('files in order:', files, flush=True)

    t0 = time.time()
    with Pool(a.procs) as pool:
        res = pool.map(pass1, [os.path.join(a.json_dir, n) for n in files])
    nodes = {}
    attack = {}
    nrec = 0
    for path, n, lst in res:
        nrec += n
        for u, val, addr in lst:
            if u not in nodes:
                idx = len(nodes) + 1
                nodes[u] = (idx, val)
                if addr is not None and addr in ips:
                    attack[idx] = ','.join(sorted(set(ips[addr]))) + ':' + addr
    print(f'pass1: {nrec} records, {len(nodes)} nodes, {len(attack)} attack netflows, {time.time()-t0:.0f}s', flush=True)
    with open(os.path.join(a.out_dir, 'nodes.tsv'), 'w') as f:
        for u, (idx, val) in nodes.items():
            f.write(f"{idx}\t{u}\t{val}\n")

    global G_NODES, G_ATTACK
    G_NODES = nodes; G_ATTACK = attack
    jobs = []
    for k, n in enumerate(files):
        jobs.append((os.path.join(a.json_dir, n), os.path.join(a.out_dir, f'edges.{k:02d}.tsv'),
                     os.path.join(a.out_dir, f'attack_hits.{k:02d}.tsv')))
    t1 = time.time()
    with Pool(a.procs) as pool:
        res = pool.map(pass2, jobs)
    tot_ev = tot_e = tot_s = 0
    for path, n_ev, n_edges, n_skip in res:
        print(f'{os.path.basename(path)}: events={n_ev} edges={n_edges} skipped={n_skip}')
        tot_ev += n_ev; tot_e += n_edges; tot_s += n_skip
    print(f'pass2: events={tot_ev} edges={tot_e} skipped={tot_s}, {time.time()-t1:.0f}s', flush=True)


if __name__ == '__main__':
    main()
