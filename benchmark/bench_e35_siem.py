#!/usr/bin/env python3
"""E35: the SIEM default (Elasticsearch) and the pipeline default (Kafka) on the E33 corpora.

Python only orchestrates. Every byte of the data path is the system itself:
Elasticsearch over its HTTP bulk/search APIs, Kafka through its own Java
producer and consumer (benchmark/e35_kafka/). Query set, window width, budgets,
checksums and the CSV schema follow docs/E33_SPEC.md so that every row lines
up with results/e33_matrix/e33.csv.

Phases (--phases, comma list; default all):
  es-ingest    bulk index over N parallel clients, batches of B docs; refresh+flush timed as `compact`;
               translog.durability=request (the default: fsync before every bulk response)
  es-ingest-async  the same into a scratch index with translog.durability=async (fsync every 5 s)
  es-q1        trailing 1% window: _count, and every hit via PIT + search_after (count + checksum)
  es-q2        term src + range ts for the 1000 shared queries: every hit (q2) and count-only (q2_count)
  es-q3        earliest-arrival reachability, client-side Dijkstra over repeated q2 calls (q3)
  es-q4        as-of: composite terms(dst) + max(ts) (q4), and + top_hits for exact ns (q4_exact)
  kafka-ingest produce 20-byte values, acks=1, 8 producer threads, producer defaults; log size on disk
  kafka-ingest-tuned  the same with linger.ms=50 and batch.size=1 MiB (what a pipeline operator sets)
  kafka-q1     consume the whole topic, filter the trailing window client side
  kafka-q2     consume the whole topic once for all 1000 queries (q2), and once for one query (q2_single)
  cleanup      stop both services, record and delete the data directories

Usage:
  python3 benchmark/bench_e35_siem.py --datasets tc-cadets-e3,wiki-talk-temporal
  python3 benchmark/bench_e35_siem.py --phases cleanup
"""
import argparse
import csv
import heapq
import json
import math
import multiprocessing as mp
import os
import shutil
import signal
import subprocess
import sys
import time

import numpy as np
import requests

REC = np.dtype([('src', '<u4'), ('dst', '<u4'), ('ts', '<u8'), ('w', '<f4')])
M64 = (1 << 64) - 1
ROOT = '/home/jding/Barry'
BASE = ROOT + '/baselines'
ES_HOME = BASE + '/elasticsearch-8.19.23'
ES_URL = 'http://127.0.0.1:9200'
ES_PIDFILE = BASE + '/es_logs/es.pid'
KAFKA_HOME = BASE + '/kafka_2.13-3.9.2'
KAFKA_CONF = KAFKA_HOME + '/config/kraft/e35-server.properties'
KAFKA_BUILD = BASE + '/e35_kafka_build'
KAFKA_BOOTSTRAP = '127.0.0.1:9092'
JAVA = '/usr/bin/java'
E33 = ROOT + '/results/e33_matrix'
BIN = '/home/jding/dataset/bin'
ES_DATA = '/home/jding/dataset/es_data'
KAFKA_DATA = '/home/jding/dataset/kafka_data'
CSV_HEADER = ('system,dataset,phase,threads,events,samples_total,samples_done,seconds,rate,'
              'median_us,p95_us,count,checksum,checksum_ok,rss_MB,note')
PAGE = 10000            # hits per search_after page / buckets per composite page


# ------------------------------------------------------------------ utilities
class Log:
    def __init__(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path, 'a')

    def __call__(self, msg):
        line = time.strftime('%Y-%m-%d %H:%M:%S ') + msg
        print(line, flush=True)
        self.f.write(line + '\n')
        self.f.flush()


def load1():
    return float(open('/proc/loadavg').read().split()[0])


def wait_for_load(threshold, log, poll=300, max_wait=10800):
    if threshold <= 0:
        return True
    t0 = time.time()
    while True:
        l = load1()
        if l < threshold:
            log(f'load gate: 1-min load {l:.2f} < {threshold}')
            return True
        if time.time() - t0 > max_wait:
            log(f'load gate: load {l:.2f} still >= {threshold} after {max_wait} s; giving up')
            return False
        log(f'load gate: 1-min load {l:.2f} >= {threshold}; retry in {poll} s')
        time.sleep(poll)


def du_bytes(path):
    """Allocated bytes (sparse preallocated files count what they use)."""
    if not os.path.exists(path):
        return 0
    out = subprocess.run(['du', '-s', '-B1', path], capture_output=True, text=True).stdout
    return int(out.split()[0]) if out else 0


def cpu_seconds(pid):
    """User+system CPU seconds of a process so far (all threads)."""
    try:
        f = open(f'/proc/{pid}/stat').read().rsplit(')', 1)[1].split()
        return (int(f[11]) + int(f[12])) / os.sysconf('SC_CLK_TCK')
    except (OSError, IndexError, ValueError):
        return 0.0


def rss_mb(pid):
    try:
        for line in open(f'/proc/{pid}/status'):
            if line.startswith('VmRSS:'):
                return int(line.split()[1]) / 1024.0
    except OSError:
        pass
    return 0.0


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def percentile(xs, q):
    if not xs:
        return 0.0
    s = sorted(xs)
    pos = q * (len(s) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


class Report:
    def __init__(self, csv_path, extra_path, log):
        self.csv_path, self.extra_path, self.log = csv_path, extra_path, log
        if not os.path.exists(csv_path):
            with open(csv_path, 'w') as f:
                f.write(CSV_HEADER + '\n')
        self.extra = json.load(open(extra_path)) if os.path.exists(extra_path) else {}

    def emit(self, system, ds, phase, threads, events, stot, sdone, sec, rate, med, p95,
             count, chk, ok, rss, note=''):
        row = [system, ds, phase, threads, events, stot, sdone, f'{sec:.6f}', f'{rate:.1f}',
               f'{med:.3f}', f'{p95:.3f}', count, chk, ok, f'{rss:.1f}', note]
        with open(self.csv_path, 'a') as f:
            csv.writer(f).writerow(row)
        self.log(f'[e35] {system:<13} {ds:<18} {phase:<10} {sdone}/{stot} {sec:9.3f} s  '
                 f'median {med:10.1f} us  p95 {p95:10.1f} us  count {count}  chk {chk}  ok {ok}  '
                 f'{note}')

    def put_extra(self, key, value):
        self.extra[key] = value
        with open(self.extra_path, 'w') as f:
            json.dump(self.extra, f, indent=1, sort_keys=True)


# ---------------------------------------------------------------- the corpus
def corpus_meta(path, limit=0):
    a = np.fromfile(path, dtype=REC, count=limit if limit > 0 else -1)
    t_min = int(a['ts'].min())
    t_max = int(a['ts'].max())
    w = max(1, (t_max - t_min) // 100)
    return dict(n=len(a), t_min=t_min, t_max=t_max, w=w,
                q1_lo=(t_max - w) if t_max >= w else 0, q1_hi=t_max)


def load_queries(ds):
    q = np.fromfile(f'{E33}/{ds}.queries', dtype='<u8').reshape(-1, 3)
    return [(int(v), int(lo), int(hi)) for v, lo, hi in q]


def load_ref(ds):
    ref, prefix = {}, {}
    p = f'{E33}/{ds}.ref'
    if os.path.exists(p):
        for line in open(p):
            ph, c, k = line.split()
            ref[ph] = (int(c), int(k))
    p = f'{E33}/{ds}.prefix'
    if os.path.exists(p):
        for line in open(p):
            ph, i, c, k = line.split()
            lst = prefix.setdefault(ph, [])
            i = int(i)
            while len(lst) < i:
                lst.append(None)
            lst[i - 1] = (int(c), int(k))
    return ref, prefix


def verdict(ref, prefix, phase, done, total, count, chk, count_only=False):
    """checksum_ok as the spec defines it: 1 match, 0 mismatch, -1 no reference.
    A budget-cut run is checked against the k-query prefix."""
    if done == total:
        r = ref.get(phase)
        if r is None:
            return -1, 'no-ref'
    else:
        lst = prefix.get(phase, [])
        r = lst[done - 1] if 0 < done <= len(lst) else None
        if r is None:
            return -1, f'no-ref (prefix {done})'
    ok_count = (r[0] == count)
    ok_chk = (r[1] == chk)
    if count_only:
        return (1 if ok_count else 0), ('count MATCH' if ok_count else f'count MISMATCH ref {r[0]}')
    if ok_count and ok_chk:
        return 1, 'MATCH' if done == total else f'MATCH (prefix {done})'
    return 0, f'MISMATCH ref count {r[0]} chk {r[1]}'


# ---------------------------------------------------------- Elasticsearch
def es_start(log):
    env = dict(os.environ)
    env.pop('ES_JAVA_HOME', None)
    env.pop('JAVA_HOME', None)
    if os.path.exists(ES_PIDFILE) and pid_alive(int(open(ES_PIDFILE).read().strip() or 0)):
        log('es: already running')
    else:
        os.makedirs(ES_DATA, exist_ok=True)
        os.makedirs(os.path.dirname(ES_PIDFILE), exist_ok=True)
        subprocess.run([ES_HOME + '/bin/elasticsearch', '-d', '-p', ES_PIDFILE], env=env,
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        try:
            r = requests.get(ES_URL + '/_cluster/health?wait_for_status=yellow&timeout=5s', timeout=10)
            if r.status_code == 200:
                v = requests.get(ES_URL, timeout=10).json()['version']
                log(f"es: up, version {v['number']} lucene {v['lucene_version']}")
                return
        except requests.RequestException:
            pass
        time.sleep(2)
    raise RuntimeError('elasticsearch did not come up')


def es_pid():
    try:
        return int(open(ES_PIDFILE).read().strip())
    except (OSError, ValueError):
        return 0


def es_stop(log):
    pid = es_pid()
    if not pid or not pid_alive(pid):
        log('es: not running')
        return
    os.kill(pid, signal.SIGTERM)
    for _ in range(120):
        if not pid_alive(pid):
            log('es: stopped')
            return
        time.sleep(1)
    os.kill(pid, signal.SIGKILL)
    log('es: killed')


def es_bulk_worker(wid, path, lo, hi, batch, index, barrier, q):
    a = np.fromfile(path, dtype=REC, count=hi - lo, offset=lo * REC.itemsize)
    s = requests.Session()
    url = f'{ES_URL}/{index}/_bulk?filter_path=errors,items.*.status'
    hdr = {'Content-Type': 'application/x-ndjson'}
    # warm the connection so the first timed batch does not pay the handshake
    s.get(ES_URL, timeout=30)
    barrier.wait()
    t_first = time.time()
    sent = retries = 0
    fmt_sec = 0.0

    def fmt(sl):
        return ''.join(
            f'{{"index":{{}}}}\n{{"src":"{sv}","dst":"{dv}","ts":{tv},"w":{wv}}}\n'
            for sv, dv, tv, wv in zip(sl['src'].tolist(), sl['dst'].tolist(),
                                      sl['ts'].tolist(), sl['w'].tolist()))

    for b0 in range(0, len(a), batch):
        sl = a[b0:b0 + batch]
        f0 = time.time()
        body = fmt(sl).encode()
        fmt_sec += time.time() - f0
        pending = sl
        for attempt in range(200):
            try:
                r = s.post(url, data=body, headers=hdr, timeout=600)
            except requests.RequestException:
                retries += 1
                time.sleep(min(5.0, 0.5 * (attempt + 1)))
                continue
            if r.status_code == 200:
                j = r.json()
                if not j.get('errors'):
                    break
                bad = [i for i, it in enumerate(j['items'])
                       if it['index'].get('status', 500) not in (200, 201)]
                pending = pending[bad]
                body = fmt(pending).encode()
                retries += 1
                time.sleep(min(5.0, 0.5 * (attempt + 1)))
            else:
                retries += 1
                time.sleep(min(5.0, 0.5 * (attempt + 1)))
        else:
            raise RuntimeError(f'worker {wid}: batch at {lo + b0} never accepted')
        sent += len(sl)
    q.put(dict(wid=wid, sent=sent, retries=retries, t_first=t_first, t_last=time.time(),
               fmt_sec=fmt_sec))


def es_ingest(rep, ds, path, meta, clients, batch, log, durability='request'):
    """durability='request' is the Elasticsearch default (fsync the translog before every bulk
    response); 'async' fsyncs every 5 s, the durability Kafka's default (no fsync) and TGSA's
    in-memory ingest also give. The async run uses its own index and drops it afterwards, so the
    queried index is always the default-durability one."""
    async_ = durability == 'async'
    index = 'e35-' + ds + ('-async' if async_ else '')
    phase = 'ingest_async' if async_ else 'ingest'
    n = meta['n']
    requests.delete(f'{ES_URL}/{index}', timeout=120)
    settings = {
        'settings': {'index': {'number_of_shards': 4, 'number_of_replicas': 0,
                               'refresh_interval': '-1', 'translog.durability': durability}},
        'mappings': {'dynamic': 'strict',
                     'properties': {'src': {'type': 'keyword'}, 'dst': {'type': 'keyword'},
                                    'ts': {'type': 'long'}, 'w': {'type': 'float'}}},
    }
    r = requests.put(f'{ES_URL}/{index}', json=settings, timeout=120)
    r.raise_for_status()
    rep.put_extra(f'es.{ds}.index_settings' + ('_async' if async_ else ''), settings)

    barrier = mp.Barrier(clients + 1)
    q = mp.Queue()
    procs = []
    for wid in range(clients):
        lo, hi = n * wid // clients, n * (wid + 1) // clients
        p = mp.Process(target=es_bulk_worker, args=(wid, path, lo, hi, batch, index, barrier, q))
        p.start()
        procs.append(p)
    barrier.wait()
    cpu0 = cpu_seconds(es_pid())
    t0 = time.time()
    results = [q.get() for _ in procs]
    for p in procs:
        p.join()
    t_last = max(r['t_last'] for r in results)
    es_cores = (cpu_seconds(es_pid()) - cpu0) / max(1e-9, t_last - t0)
    sent = sum(r['sent'] for r in results)
    retries = sum(r['retries'] for r in results)
    fmt_sec = max(r['fmt_sec'] for r in results)
    ingest_sec = t_last - t0
    log(f'es ingest: {sent} docs in {ingest_sec:.2f} s ({sent / ingest_sec:,.0f}/s); '
        f'retries {retries}; elasticsearch kept {es_cores:.1f} cores busy; client formatting took at '
        f'most {fmt_sec:.1f} s of that per worker')

    # compact = make the data searchable and durable: refresh_interval back to the default, refresh, flush
    c0 = time.time()
    requests.put(f'{ES_URL}/{index}/_settings', json={'index': {'refresh_interval': None}},
                 timeout=600).raise_for_status()
    requests.post(f'{ES_URL}/{index}/_refresh', timeout=1800).raise_for_status()
    requests.post(f'{ES_URL}/{index}/_flush', timeout=1800).raise_for_status()
    compact_sec = time.time() - c0
    st = requests.get(f'{ES_URL}/{index}/_stats/store,docs,segments', timeout=120).json()['_all']['primaries']
    store = st['store']['size_in_bytes']
    docs = st['docs']['count']
    segs = st['segments']['count']
    du = du_bytes(ES_DATA)
    rss = rss_mb(es_pid())
    log(f'es compact: {compact_sec:.2f} s; docs.count {docs} (expected {n}); store {store / 2**30:.2f} GiB; '
        f'du {du / 2**30:.2f} GiB; segments {segs}; es rss {rss:.0f} MB')
    rep.put_extra(f'es.{ds}.{phase}', dict(docs=docs, durability=durability, expected=n, store_bytes=store, du_bytes=du,
                                            segments=segs, retries=retries, clients=clients, batch=batch,
                                            ingest_seconds=ingest_sec, compact_seconds=compact_sec,
                                            client_fmt_seconds_max=fmt_sec, rss_mb=rss,
                                            es_cores_busy=es_cores, bytes_per_event=store / max(1, docs)))
    note = (f'bulk {clients} clients x {batch} docs, refresh_interval=-1 during bulk, 4 shards, '
            f'translog.durability={durability}; es {es_cores:.1f} cores busy')
    if docs != n:
        note += f'; docs.count {docs} != {n}'
    rep.emit('elasticsearch', ds, phase, clients, n, 0, 0, ingest_sec, sent / ingest_sec,
             0, 0, 0, 0, -1, rss, note)
    rep.emit('elasticsearch', ds, 'compact_async' if async_ else 'compact', clients, n, 0, 0, compact_sec,
             0, 0, 0, 0, 0, -1, rss, f'refresh+flush; store {store} B ({store / 2**30:.2f} GiB), {segs} segments')
    if async_:
        requests.delete(f'{ES_URL}/{index}', timeout=600)


class EsQuery:
    """One HTTP session, one PIT for the hit-retrieving phases."""

    def __init__(self, index):
        self.index = index
        self.s = requests.Session()
        self.pit = None
        self.took = 0

    def open_pit(self):
        r = self.s.post(f'{ES_URL}/{self.index}/_pit?keep_alive=30m', timeout=120)
        r.raise_for_status()
        self.pit = r.json()['id']

    def close_pit(self):
        if self.pit:
            self.s.delete(f'{ES_URL}/_pit', json={'id': self.pit}, timeout=120)
            self.pit = None

    def clear_cache(self):
        self.s.post(f'{ES_URL}/{self.index}/_cache/clear', timeout=120)

    def count(self, query):
        r = self.s.get(f'{ES_URL}/{self.index}/_count', json={'query': query}, timeout=600)
        r.raise_for_status()
        return r.json()['count']

    def hits(self, query, fields):
        """Every hit of `query`, as a list of tuples of the requested docvalue fields (ints)."""
        out = []
        body = {'size': PAGE, 'query': query, 'pit': {'id': self.pit, 'keep_alive': '30m'},
                'sort': ['_shard_doc'], '_source': False, 'docvalue_fields': fields,
                'track_total_hits': False}
        url = f'{ES_URL}/_search?filter_path=pit_id,took,hits.hits.fields,hits.hits.sort'
        while True:
            r = self.s.post(url, json=body, timeout=600)
            r.raise_for_status()
            j = r.json()
            self.took += j.get('took', 0)
            self.pit = j.get('pit_id', self.pit)
            hs = j.get('hits', {}).get('hits', [])
            for h in hs:
                f = h['fields']
                out.append(tuple(int(f[k][0]) for k in fields))
            if len(hs) < PAGE:
                return out
            body['search_after'] = hs[-1]['sort']

    def asof(self, v, t, exact):
        """(dst, max ts) for every distinct dst among out-events of v with ts <= t, via a
        composite terms(dst) aggregation. exact=False: max(ts) (a double in Elasticsearch;
        exact up to 2^53); exact=True: top_hits size 1 sorted by ts desc (exact long)."""
        sub = ({'m': {'top_hits': {'size': 1, 'sort': [{'ts': 'desc'}], '_source': False,
                                   'docvalue_fields': ['ts']}}}
               if exact else {'m': {'max': {'field': 'ts'}}})
        body = {'size': 0,
                'query': {'bool': {'filter': [{'term': {'src': str(v)}},
                                              {'range': {'ts': {'lte': t}}}]}},
                'aggs': {'d': {'composite': {'size': PAGE,
                                             'sources': [{'dst': {'terms': {'field': 'dst'}}}]},
                               'aggs': sub}}}
        fp = ('took,aggregations.d.after_key,aggregations.d.buckets.key.dst,'
              + ('aggregations.d.buckets.m.hits.hits.fields.ts' if exact
                 else 'aggregations.d.buckets.m.value'))
        url = f'{ES_URL}/{self.index}/_search?filter_path={fp}'
        out = []
        while True:
            r = self.s.post(url, json=body, timeout=600)
            r.raise_for_status()
            j = r.json()
            self.took += j.get('took', 0)
            agg = j.get('aggregations', {}).get('d', {})
            for b in agg.get('buckets', []):
                d = int(b['key']['dst'])
                if exact:
                    mt = int(b['m']['hits']['hits'][0]['fields']['ts'][0])
                else:
                    mt = int(b['m']['value'])
                out.append((d, mt))
            if 'after_key' not in agg or len(agg.get('buckets', [])) < PAGE:
                return out
            body['aggs']['d']['composite']['after'] = agg['after_key']


def q2_query(v, lo, hi):
    return {'bool': {'filter': [{'term': {'src': str(v)}}, {'range': {'ts': {'gte': lo, 'lte': hi}}}]}}


def run_latency(qs, budget, one, log):
    """docs/E33_SPEC.md's latency driver: time each query; after 20, project; run as many as
    fit the budget; a hard deadline abandons a query that overruns (not counted)."""
    lat, cum = [], []
    count = chk = 0
    N = len(qs)
    n_run = N
    note = ''
    t0 = time.time()
    done = 0
    extra = []
    for i in range(N):
        if i >= n_run:
            break
        if budget > 0 and time.time() - t0 > budget * 1.25:
            note = (note + '; ' if note else '') + f'deadline at {done} queries'
            break
        q0 = time.time()
        res = one(qs[i], budget * 1.25 - (q0 - t0) if budget > 0 else 0)
        if res is None:
            note = (note + '; ' if note else '') + f'deadline inside query {done + 1}'
            break
        c, k, x = res
        lat.append((time.time() - q0) * 1e6)
        count += c
        chk = (chk + k) & M64
        cum.append((count, chk))
        extra.append(x)
        done += 1
        if i + 1 == 20 and N > 20 and budget > 0:
            per = (time.time() - t0) / 20.0
            projected = per * N
            if projected > budget:
                n_run = min(N, max(20, int(budget / per)))
                note = f'budget: projected {projected:.0f} s for {N} queries; running {n_run}'
                log(note)
    return dict(done=done, seconds=time.time() - t0, lat=lat, count=count, chk=chk, cum=cum,
                note=note, extra=extra)


def es_queries(rep, ds, meta, phases, budget, log, gate):
    index = 'e35-' + ds
    n = meta['n']
    ref, prefix = load_ref(ds)
    qs = load_queries(ds)
    N = len(qs)
    eq = EsQuery(index)
    docs = requests.get(f'{ES_URL}/{index}/_count', timeout=120).json()['count']
    if docs != n:
        log(f'WARNING: index holds {docs} docs, corpus has {n}')

    def emit_lat(phase, r, ok, verd, extra_note='', took=None):
        med, p95 = percentile(r['lat'], 0.5), percentile(r['lat'], 0.95)
        if took:
            extra_note += (f'; es-reported took median {percentile(took, 0.5):.0f} ms '
                           f'p95 {percentile(took, 0.95):.0f} ms')
        note = '; '.join(x for x in [verd, r['note'], extra_note] if x)
        rep.emit('elasticsearch', ds, phase, 1, n, N, r['done'], r['seconds'],
                 r['done'] / r['seconds'] if r['seconds'] > 0 else 0, med, p95, r['count'], r['chk'],
                 ok, rss_mb(es_pid()), note)
        rep.put_extra(f'es.{ds}.{phase}', dict(done=r['done'], seconds=r['seconds'], median_us=med,
                                                p95_us=p95, count=r['count'], chk=r['chk'], ok=ok,
                                                es_took_ms_total=eq.took,
                                                es_took_ms_median=percentile(took, 0.5) if took else None,
                                                es_took_ms_p95=percentile(took, 0.95) if took else None,
                                                note=note))

    # ------------------------------------------------------------------ q1
    if 'es-q1' in phases and gate():
        eq.clear_cache()
        rq = {'range': {'ts': {'gte': meta['q1_lo'], 'lte': meta['q1_hi']}}}
        t0 = time.time()
        c = eq.count(rq)
        sec = time.time() - t0
        ok, verd = verdict(ref, prefix, 'q1', 1, 1, c, 0, count_only=True)
        rep.emit('elasticsearch', ds, 'q1_count', 1, n, 1, 1, sec, 0, 0, 0, c, 0, ok, rss_mb(es_pid()),
                 f'_count on range ts; {verd}')
        eq.clear_cache()
        eq.open_pit()
        eq.took = 0
        t0 = time.time()
        hs = eq.hits(rq, ['src', 'dst', 'ts'])
        sec = time.time() - t0
        chk = 0
        for s_, d_, t_ in hs:
            chk = (chk + (t_ ^ (d_ << 32) ^ s_)) & M64
        ok, verd = verdict(ref, prefix, 'q1', 1, 1, len(hs), chk)
        eq.close_pit()
        rep.emit('elasticsearch', ds, 'q1', 1, n, 1, 1, sec, 0, 0, 0, len(hs), chk, ok, rss_mb(es_pid()),
                 f'every hit via PIT + search_after, {PAGE}/page, es took {eq.took} ms; {verd}')

    # ------------------------------------------------------------------ q2
    if 'es-q2' in phases and gate():
        eq.clear_cache()
        eq.open_pit()
        eq.took = 0

        def one_q2(q, _left):
            v, lo, hi = q
            tk = eq.took
            hs = eq.hits(q2_query(v, lo, hi), ['dst', 'ts'])
            k = 0
            for d_, t_ in hs:
                k = (k + (t_ ^ d_)) & M64
            return len(hs), k, eq.took - tk

        r = run_latency(qs, budget, one_q2, log)
        eq.close_pit()
        ok, verd = verdict(ref, prefix, 'q2', r['done'], N, r['count'], r['chk'])
        emit_lat('q2', r, ok, verd, f'every hit via PIT + search_after, {PAGE}/page', took=r['extra'])

        eq.clear_cache()
        eq.took = 0

        def one_q2c(q, _left):
            v, lo, hi = q
            c = eq.count(q2_query(v, lo, hi))
            return c, 0, c

        r = run_latency(qs, budget, one_q2c, log)
        ok, verd = verdict(ref, prefix, 'q2', r['done'], N, r['count'], 0, count_only=True)
        emit_lat('q2_count', r, ok, verd, '_count only (no events delivered)')

    # ------------------------------------------------------------------ q3
    if 'es-q3' in phases and gate():
        eq.clear_cache()
        eq.open_pit()
        eq.took = 0

        def one_q3(q, left):
            v, lo, hi = q
            label = {v: lo}
            heap = [(lo, v)]
            calls = 0
            depth = {v: 0}
            maxdepth = 0
            tk = eq.took
            t_start = time.time()
            while heap:
                au, u = heapq.heappop(heap)
                if label[u] != au:
                    continue
                hs = eq.hits(q2_query(u, au, hi), ['dst', 'ts'])
                calls += 1
                if (calls & 63) == 0 and left > 0 and time.time() - t_start > left:
                    return None
                for d, ts in hs:
                    a = label.get(d)
                    if a is None or ts < a:
                        label[d] = ts
                        depth[d] = depth[u] + 1
                        maxdepth = max(maxdepth, depth[d])
                        heapq.heappush(heap, (ts, d))
            c = k = 0
            for d, a in label.items():
                if d != v:
                    c += 1
                    k = (k + (d ^ a)) & M64
            return c, k, (calls, maxdepth, eq.took - tk)

        r = run_latency(qs, budget, one_q3, log)
        eq.close_pit()
        ok, verd = verdict(ref, prefix, 'q3', r['done'], N, r['count'], r['chk'])
        calls = [x[0] for x in r['extra']]
        depths = [x[1] for x in r['extra']]
        hop_note = (f'client-side earliest-arrival search, one term+range call per expanded vertex: '
                    f'{sum(calls)} calls over {r["done"]} queries, median {percentile(calls, 0.5):.0f} '
                    f'p95 {percentile(calls, 0.95):.0f} max {max(calls) if calls else 0} calls/query, '
                    f'median depth {percentile(depths, 0.5):.0f} max {max(depths) if depths else 0} hops')
        emit_lat('q3', r, ok, verd, hop_note, took=[x[2] for x in r['extra']])
        rep.put_extra(f'es.{ds}.q3_calls', dict(calls_total=sum(calls), calls_median=percentile(calls, 0.5),
                                                 calls_p95=percentile(calls, 0.95),
                                                 calls_max=max(calls) if calls else 0,
                                                 depth_median=percentile(depths, 0.5),
                                                 depth_max=max(depths) if depths else 0))

    # ------------------------------------------------------------------ q4
    if 'es-q4' in phases and gate():
        for exact, phase in ((False, 'q4'), (True, 'q4_exact')):
            eq.clear_cache()
            eq.took = 0

            def one_q4(q, _left, exact=exact):
                v, lo, hi = q
                tk = eq.took
                pairs = eq.asof(v, hi, exact)
                k = 0
                for d, mt in pairs:
                    k = (k + ((d << 20) ^ mt ^ v)) & M64
                return len(pairs), k, eq.took - tk

            r = run_latency(qs, budget, one_q4, log)
            ok, verd = verdict(ref, prefix, 'q4', r['done'], N, r['count'], r['chk'])
            if exact:
                desc = f'composite terms(dst) {PAGE}/page + top_hits(1, ts desc): exact 64-bit max'
            else:
                desc = (f'composite terms(dst) {PAGE}/page + max(ts): the max aggregation returns a '
                        f'double, exact only below 2^53')
                if ok == 0:
                    okc, verdc = verdict(ref, prefix, 'q4', r['done'], N, r['count'], 0, count_only=True)
                    if okc == 1:
                        verd = 'count MATCH, checksum differs (double-rounded ns timestamps)'
            emit_lat(phase, r, ok, verd, desc, took=r['extra'])


# ------------------------------------------------------------------ Kafka
def kafka_env():
    env = dict(os.environ)
    env['LOG_DIR'] = BASE + '/kafka_logs'
    env['KAFKA_HEAP_OPTS'] = '-Xms4g -Xmx4g'
    env['JAVA_HOME'] = os.path.dirname(os.path.dirname(os.path.realpath(JAVA)))
    return env


def kafka_pid():
    out = subprocess.run(['pgrep', '-f', 'kafka.Kafka .*e35-server.properties'],
                         capture_output=True, text=True).stdout.split()
    for pid in out:
        try:
            if open(f'/proc/{pid}/comm').read().strip() == 'java':
                return int(pid)
        except OSError:
            pass
    return 0


def kafka_cli(tool, *args, check=True):
    return subprocess.run([f'{KAFKA_HOME}/bin/{tool}', '--bootstrap-server', KAFKA_BOOTSTRAP, *args],
                          env=kafka_env(), capture_output=True, text=True, check=check)


def kafka_start(log):
    if kafka_pid():
        log('kafka: already running')
    else:
        os.makedirs(KAFKA_DATA, exist_ok=True)
        os.makedirs(BASE + '/kafka_logs', exist_ok=True)
        if not os.path.exists(KAFKA_DATA + '/meta.properties'):
            uuid = subprocess.run([KAFKA_HOME + '/bin/kafka-storage.sh', 'random-uuid'], env=kafka_env(),
                                  capture_output=True, text=True, check=True).stdout.strip()
            subprocess.run([KAFKA_HOME + '/bin/kafka-storage.sh', 'format', '-t', uuid, '-c', KAFKA_CONF],
                           env=kafka_env(), check=True, capture_output=True)
            log(f'kafka: formatted KRaft storage, cluster id {uuid}')
        subprocess.run([KAFKA_HOME + '/bin/kafka-server-start.sh', '-daemon', KAFKA_CONF], env=kafka_env(),
                       check=True, capture_output=True)
    for _ in range(60):
        if kafka_cli('kafka-broker-api-versions.sh', check=False).returncode == 0:
            log('kafka: up')
            return
        time.sleep(2)
    raise RuntimeError('kafka did not come up')


def kafka_stop(log):
    pid = kafka_pid()
    if not pid:
        log('kafka: not running')
        return
    subprocess.run([KAFKA_HOME + '/bin/kafka-server-stop.sh'], env=kafka_env(), capture_output=True)
    for _ in range(120):
        if not pid_alive(pid):
            log('kafka: stopped')
            return
        time.sleep(1)
    os.kill(pid, signal.SIGKILL)
    log('kafka: killed')


def kafka_java(cls, *args):
    cmd = [JAVA, '-Xms2g', '-Xmx2g', '-cp', f'{KAFKA_HOME}/libs/*:{KAFKA_BUILD}',
           f'-Dlog4j.configuration=file:{KAFKA_HOME}/config/tools-log4j.properties', cls, *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f'{cls} failed: {r.stderr[-2000:]}')
    line = [l for l in r.stdout.splitlines() if l.startswith('{')][-1]
    return json.loads(line)


def kafka_topic_bytes(topic):
    """(bytes of the *.log segment files, allocated bytes of the partition directories
    including the preallocated offset/time index files)."""
    logs = alloc = 0
    for d in os.listdir(KAFKA_DATA):
        if d.startswith(topic + '-') and d[len(topic) + 1:].isdigit():
            full = os.path.join(KAFKA_DATA, d)
            alloc += du_bytes(full)
            for f in os.listdir(full):
                if f.endswith('.log'):
                    logs += os.path.getsize(os.path.join(full, f))
    return logs, alloc


def kafka_ingest(rep, ds, path, meta, threads, log, tuned=False):
    topic = 'e35-' + ds + ('-tuned' if tuned else '')
    phase = 'ingest_tuned' if tuned else 'ingest'
    n = meta['n']
    if topic in kafka_cli('kafka-topics.sh', '--list').stdout.split():
        kafka_cli('kafka-topics.sh', '--delete', '--topic', topic)
        for _ in range(120):
            if topic not in kafka_cli('kafka-topics.sh', '--list').stdout.split():
                break
            time.sleep(1)
        time.sleep(2)
    kafka_cli('kafka-topics.sh', '--create', '--topic', topic, '--partitions', '8',
              '--replication-factor', '1', '--config', 'retention.ms=-1', '--config', 'retention.bytes=-1')
    extra = ['--linger-ms', '50', '--batch-size', '1048576'] if tuned else []
    cpu0, t0 = cpu_seconds(kafka_pid()), time.time()
    j = kafka_java('E35Producer', '--bin', path, '--topic', topic, '--threads', str(threads), '--acks', '1',
                   '--limit', str(n), *extra)
    cores = (cpu_seconds(kafka_pid()) - cpu0) / max(1e-9, time.time() - t0)
    log(f'kafka produce: {j}; broker kept about {cores:.1f} cores busy (over the JVM run, incl. startup)')
    time.sleep(3)
    tb, alloc = kafka_topic_bytes(topic)
    du = du_bytes(KAFKA_DATA)
    rss = rss_mb(kafka_pid())
    offs = kafka_cli('kafka-get-offsets.sh', '--topic', topic).stdout
    end_total = sum(int(l.rsplit(':', 1)[1]) for l in offs.split() if ':' in l)
    log(f'kafka: topic {topic} log segments {tb / 2**30:.2f} GiB ({tb / max(1, n):.1f} B/event), partition dirs '
        f'{alloc / 2**30:.2f} GiB allocated, kafka_data du {du / 2**30:.2f} GiB, end offsets sum {end_total} '
        f'(expected {n}), broker rss {rss:.0f} MB')
    rep.put_extra(f'kafka.{ds}.{phase}', dict(**j, log_bytes=tb, alloc_bytes=alloc, du_bytes=du,
                                                end_offsets=end_total, expected=n, rss_mb=rss,
                                                bytes_per_event=tb / max(1, n), broker_cores_busy=cores))
    note = (f'Java producer, acks=1, {threads} producers, null key, 20-byte values, 8 partitions, '
            + ('linger.ms=50 batch.size=1MiB' if tuned else 'linger.ms/batch.size default (0/16384)')
            + f'; log segments {tb} B ({tb / 2**30:.2f} GiB, {tb / max(1, n):.1f} B/event)')
    if j['errors'] or end_total != n:
        note += f'; errors {j["errors"]}, end offsets {end_total}'
    rep.emit('kafka', ds, phase, threads, n, 0, 0, j['seconds'], j['rate'], 0, 0, 0, 0, -1, rss, note)
    if tuned:
        kafka_cli('kafka-topics.sh', '--delete', '--topic', topic)


def kafka_queries(rep, ds, meta, phases, log, gate):
    topic = 'e35-' + ds
    n = meta['n']
    ref, prefix = load_ref(ds)
    N = len(load_queries(ds))
    if 'kafka-q1' in phases and gate():
        j = kafka_java('E35Consumer', '--topic', topic, '--mode', 'q1', '--lo', str(meta['q1_lo']),
                       '--hi', str(meta['q1_hi']))
        log(f'kafka q1: {j}')
        ok, verd = verdict(ref, prefix, 'q1', 1, 1, j['count'], int(j['checksum']))
        rep.emit('kafka', ds, 'q1', j['partitions'], n, 1, 1, j['seconds'], 0, j['seconds'] * 1e6,
                 j['seconds'] * 1e6, j['count'], int(j['checksum']), ok, rss_mb(kafka_pid()),
                 f'full consume of {j["scanned"]} records, range filter client side; {verd}')
        rep.put_extra(f'kafka.{ds}.q1', j)
    if 'kafka-q2' in phases and gate():
        j = kafka_java('E35Consumer', '--topic', topic, '--mode', 'q2', '--queries', f'{E33}/{ds}.queries')
        log(f'kafka q2: {j}')
        ok, verd = verdict(ref, prefix, 'q2', N, N, j['count'], int(j['checksum']))
        per = j['seconds'] / N
        rep.emit('kafka', ds, 'q2', j['partitions'], n, N, N, j['seconds'], N / j['seconds'],
                 j['seconds'] * 1e6, j['seconds'] * 1e6, j['count'], int(j['checksum']), ok,
                 rss_mb(kafka_pid()),
                 f'one full consume of {j["scanned"]} records answers all {N} queries '
                 f'(src+range filter client side); the per-query cost is the full scan, not {per * 1e6:.0f} us; {verd}')
        rep.put_extra(f'kafka.{ds}.q2', j)
        j1 = kafka_java('E35Consumer', '--topic', topic, '--mode', 'q2', '--queries',
                        f'{E33}/{ds}.queries', '--qindex', '0')
        log(f'kafka q2 single: {j1}')
        pre = prefix.get('q2', [])
        okp = 1 if (pre and pre[0] and pre[0] == (j1['count'], int(j1['checksum']))) else (0 if pre else -1)
        rep.emit('kafka', ds, 'q2_single', j1['partitions'], n, 1, 1, j1['seconds'], 1 / j1['seconds'],
                 j1['seconds'] * 1e6, j1['seconds'] * 1e6, j1['count'], int(j1['checksum']), okp,
                 rss_mb(kafka_pid()),
                 f'one query = one full consume of {j1["scanned"]} records; checked against the 1-query prefix')
        rep.put_extra(f'kafka.{ds}.q2_single', j1)


# ------------------------------------------------------------------ cleanup
def cleanup(rep, log):
    es_stop(log)
    kafka_stop(log)
    sizes = {'es_data_bytes': du_bytes(ES_DATA), 'kafka_data_bytes': du_bytes(KAFKA_DATA)}
    log(f'cleanup: es_data {sizes["es_data_bytes"] / 2**30:.2f} GiB, kafka_data '
        f'{sizes["kafka_data_bytes"] / 2**30:.2f} GiB; deleting both')
    rep.put_extra('cleanup', dict(**sizes, deleted=time.strftime('%Y-%m-%dT%H:%M:%S')))
    for d in (ES_DATA, KAFKA_DATA):
        shutil.rmtree(d, ignore_errors=True)
    log('cleanup: done')


# ------------------------------------------------------------------ main
ALL_PHASES = ['es-ingest', 'es-ingest-async', 'es-q1', 'es-q2', 'es-q3', 'es-q4', 'kafka-ingest', 'kafka-ingest-tuned',
              'kafka-q1', 'kafka-q2']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--datasets', default='tc-cadets-e3,wiki-talk-temporal')
    ap.add_argument('--phases', default=','.join(ALL_PHASES))
    ap.add_argument('--out', default=ROOT + '/results/e35_siem')
    ap.add_argument('--clients', type=int, default=8)
    ap.add_argument('--batch', type=int, default=5000)
    ap.add_argument('--budget', type=float, default=1800.0, help='seconds per latency phase (E33 default)')
    ap.add_argument('--load-gate', type=float, default=4.0, help='run timed phases only below this 1-min load; 0 disables')
    ap.add_argument('--keep-running', action='store_true', help='leave the services up at exit')
    ap.add_argument('--limit', type=int, default=0, help='smoke tests only: first N records of each corpus')
    a = ap.parse_args()

    os.makedirs(a.out + '/logs', exist_ok=True)
    log = Log(a.out + '/logs/bench_e35_siem.log')
    rep = Report(a.out + '/e35.csv', a.out + '/e35_extra.json', log)
    phases = set(a.phases.split(','))
    log(f'start: datasets {a.datasets} phases {sorted(phases)} budget {a.budget} load-gate {a.load_gate}')

    def gate():
        return wait_for_load(a.load_gate, log)

    if 'cleanup' in phases:
        cleanup(rep, log)
        return

    es_phases = {p for p in phases if p.startswith('es-')}
    kafka_phases = {p for p in phases if p.startswith('kafka-')}
    try:
        for ds in a.datasets.split(','):
            path = f'{BIN}/{ds}.bin'
            meta = corpus_meta(path, a.limit)
            log(f'{ds}: {meta}')
            rep.put_extra(f'meta.{ds}', meta)
            if es_phases:
                es_start(log)
                if 'es-ingest' in phases and gate():
                    es_ingest(rep, ds, path, meta, a.clients, a.batch, log)
                if 'es-ingest-async' in phases and gate():
                    es_ingest(rep, ds, path, meta, a.clients, a.batch, log, durability='async')
                es_queries(rep, ds, meta, es_phases, a.budget, log, gate)
            if kafka_phases:
                kafka_start(log)
                if 'kafka-ingest' in phases and gate():
                    kafka_ingest(rep, ds, path, meta, a.clients, log)
                if 'kafka-ingest-tuned' in phases and gate():
                    kafka_ingest(rep, ds, path, meta, a.clients, log, tuned=True)
                kafka_queries(rep, ds, meta, kafka_phases, log, gate)
    finally:
        if not a.keep_running:
            if es_phases:
                es_stop(log)
            if kafka_phases:
                kafka_stop(log)
    log('done')


if __name__ == '__main__':
    main()
