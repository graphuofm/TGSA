#!/usr/bin/env python3
"""Compute every number the SIGMOD paper quotes from the result CSVs and
print them as a table, plus the \\newcommand lines for the preamble macros.

    python3 scripts/fill_numbers.py

The paper's headline numbers live in main.tex as macros (\\nIngestGapLo etc.)
and the body text quotes the per-system figures this script prints. Run it
after the queue (scripts/run_revision.sh) and paste; never type a number into
the paper that this script did not print.
"""
import csv, os, sys, glob, re
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'paper_SIGMOD', 'figures'))
from figstyle import load_e33, e33_cell, CORPORA, SHORT, SYS_ORDER, fnum, med, load, sysk  # noqa

rows = load_e33()
COMP = ['graphone', 'livegraph', 'rocksdb', 'duckdb', 'stinger', 'sortledton']

def fmt(x, d=1):
    return 'n/a' if x is None else (f'{x:.{d}f}')

def ratio(a, b):
    return None if not a or not b else a / b

print('=' * 78)
print('INGEST (M events/s, eight threads)')
hdr = f"{'corpus':12s}" + ''.join(f'{s:>12s}' for s in SYS_ORDER) + f"{'best comp.':>12s}{'TGSR/best':>10s}"
print(hdr)
gap = []
tg_rates = []
for c in CORPORA:
    line = f'{SHORT[c]:12s}'
    best = None; bests = ''
    for s in SYS_ORDER:
        r = e33_cell(rows, s, c, 'ingest')
        v = fnum(r['rate']) / 1e6 if r else None
        flag = '*' if r and 'DNF' in r.get('note', '') else ''
        line += f'{(fmt(v, 2) + flag) if v is not None else "-":>12s}'
        if s != 'tgsr' and v and (best is None or v > best):
            best, bests = v, s
    t = e33_cell(rows, 'tgsr', c, 'ingest')
    tv = fnum(t['rate']) / 1e6 if t else None
    if tv:
        tg_rates.append(tv)
    g = ratio(tv, best)
    if g:
        gap.append(g)
    line += f'{(bests + " " + fmt(best, 2)) if best else "-":>12s}{fmt(g, 1):>10s}'
    print(line)
print('  * = did not finish; rate sustained before the cutoff')

print('\nMEMORY (resident bytes per event)')
for c in CORPORA:
    line = f'{SHORT[c]:12s}'
    for s in SYS_ORDER:
        r = e33_cell(rows, s, c, 'ingest')
        v = fnum(r['rss_MB']) * 1e6 / fnum(r['events']) if r and fnum(r['rss_MB']) > 0 else None
        line += f'{fmt(v, 0):>12s}'
    print(line)

print('\nQUERIES (q1: ms for the one query; q2..q4: median us); ratio = system / TGSR')
qgap = []
for phase in ['q1', 'q2', 'q3', 'q4']:
    print(f'-- {phase}')
    for c in CORPORA:
        t = e33_cell(rows, 'tgsr', c, phase)
        tv = (fnum(t['seconds']) * 1e3 if phase == 'q1' else fnum(t['median_us'])) if t else None
        line = f'{SHORT[c]:12s} TGSR {fmt(tv, 3):>10s} |'
        for s in ['graphone', 'livegraph', 'rocksdb', 'duckdb']:
            r = e33_cell(rows, s, c, phase)
            if r is None or r.get('note', '').startswith('n/a'):
                line += f' {s}: -'
                continue
            v = fnum(r['seconds']) * 1e3 if phase == 'q1' else fnum(r['median_us'])
            done, tot = fnum(r.get('samples_done')), fnum(r.get('samples_total'))
            tag = '' if not tot or done >= tot else f'({int(done)}/{int(tot)})'
            ok = '' if r.get('checksum_ok') in ('1', 1) else '!CHK'
            g = ratio(v, tv)
            if g:
                qgap.append((phase, c, s, g))
            line += f' {s}: {fmt(v, 2)} x{fmt(g, 1)}{tag}{ok}'
        print(line)

print('\nBFS seconds (harness BFS; GraphOne own kernel from graphone.csv)')
for c in CORPORA:
    line = f'{SHORT[c]:12s}'
    for s in ['tgsr', 'livegraph']:
        r = e33_cell(rows, s, c, 'bfs')
        line += f" {s}: {fmt(fnum(r['seconds']), 3) if r else '-'}"
    print(line)

# ---- macros
def lohi(vals):
    return (min(vals), max(vals)) if vals else (None, None)
ig_lo, ig_hi = lohi(gap)
q_vals = [g for (_, _, _, g) in qgap]
q_lo, q_hi = lohi(q_vals)
ti_lo, ti_hi = lohi(tg_rates)
print('\n' + '=' * 78)
print('PREAMBLE MACROS (paste into main.tex)')
print(f'\\newcommand{{\\nIngestMin}}{{{fmt(ti_lo, 1)}}}')
print(f'\\newcommand{{\\nIngestMax}}{{{fmt(ti_hi, 1)}}}')
print(f'\\newcommand{{\\nIngestGapLo}}{{{fmt(ig_lo, 1)}}}')
print(f'\\newcommand{{\\nIngestGapHi}}{{{fmt(ig_hi, 0)}}}')
print(f'\\newcommand{{\\nQueryGapLo}}{{{fmt(q_lo, 1)}}}')
print(f'\\newcommand{{\\nQueryGapHi}}{{{fmt(q_hi, 0)}}}')
if qgap:
    worst = min(qgap, key=lambda x: x[3])
    print(f'  smallest query advantage: {worst}')
    print(f'  largest query advantage: {max(qgap, key=lambda x: x[3])}')

# ---- structural: TGSR / CSR on E13
e13 = []
for f in glob.glob(os.path.join(HERE, '..', 'results', 'e13_storage', 'e13_*.csv')):
    e13 += list(csv.DictReader(open(f)))
rat = []
for c in CORPORA:
    for alg in ['bfs', 'pr', 'cc']:
        t = [fnum(r['storage_sec']) for r in e13 if r['dataset'] == c and r['algo'] == alg and sysk(r['engine']) == 'tgsr']
        s = [fnum(r['storage_sec']) for r in e13 if r['dataset'] == c and r['algo'] == alg and r['engine'] == 'csr']
        if t and s and med(s) > 0:
            rat.append((c, alg, med(t) / med(s)))
if rat:
    print('\nSTRUCTURAL TGSR/CSR storage share:', [(SHORT[c], a, round(x, 2)) for c, a, x in rat])
    print(f'\\newcommand{{\\nStructuralGap}}{{{med([x for _, _, x in rat]):.1f}}}')
