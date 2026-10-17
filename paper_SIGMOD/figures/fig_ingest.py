#!/usr/bin/env python3
"""fig_ingest.pdf: the throughput figure. (a) Ingestion on eight threads,
seven systems on the four aligned corpora, like for like. (b) Ingestion
while four readers query the same store (E26, eight writers, settled
windows, zero mismatches). (c) Resident bytes per event.
Source: the aligned matrix (results/e33_matrix) plus the Stinger and
Sortledton benches; GraphOne's memory is its harness peak RSS less its
in-memory input buffer (24 B per event), parsed from the queue's time -v
output when present."""
from figstyle import *
import re, glob

def graphone_rss_mb():
    """corpus basename -> peak RSS MB of the GraphOne run, less input buffer."""
    out = {}
    for f in glob.glob(R + 'revision_logs/e33_*_graphone.time'):
        short = re.search(r'e33_(\w+)_graphone', f).group(1)
        ds = {'wiki': 'wiki-talk-temporal', 'cadets': 'tc-cadets-e3',
              'theia': 'tc-theia-e3', 'lanl': 'lanl-auth'}.get(short)
        m = re.search(r'Maximum resident set size \(kbytes\): (\d+)', open(f).read())
        if ds and m:
            out[ds] = int(m.group(1)) / 1e3
    return out

def fig_ingest():
    rows = load_e33()
    go_rss = graphone_rss_mb()
    fig, (a, c, b) = plt.subplots(1, 3, figsize=(7.0, 2.15),
                                  gridspec_kw=dict(width_ratios=[1.3, 0.9, 1.05]))

    def ingest(s, c):
        r = e33_cell(rows, s, c, 'ingest')
        if r is None:
            return None, ''
        rate = fnum(r['rate']) / 1e6
        note = ''
        if 'DNF' in r.get('note', ''):
            note = 'did not finish'
        elif s == 'sortledton':
            note = 'pairs/s'
        return rate, note
    grouped_bars(a, CORPORA, SYS_ORDER, ingest, log=True,
                 annotate=lambda y: f'{y:.1f}' if y >= 1 else f'{y:.2f}')
    a.set_ylabel('M events/s (log)')
    a.set_ylim(0.005, 400)
    panel_label(a, '(a) Ingestion')

    # ---- (b) ingestion under four concurrent readers (E26, 8:4 mix)
    from collections import defaultdict
    conc = defaultdict(list)
    for r in try_load('e26_concurrent/e26.csv'):
        if r['writers'] != '8' or r['readers'] != '4' or r['system'] == 'livegraph':
            continue
        if fnum(r.get('mismatches')) != 0:
            continue
        conc[(sysk(r['system']), r['dataset'])].append(fnum(r['ingest_Meps']))
    grouped_bars(c, CORPORA, ['tgsr', 'graphone', 'livegraph'],
                 lambda s, cc: (med(conc[(s, cc)]) if conc[(s, cc)] else None,
                                '' if conc[(s, cc)] else 'n/a'),
                 log=True, annotate=lambda y: f'{y:.1f}')
    c.set_ylabel('M events/s (log)')
    c.set_ylim(0.005, 400)
    panel_label(c, '(b) Under 4 readers')

    def bpe(s, c):
        r = e33_cell(rows, s, c, 'ingest')
        if r is None:
            return None, ''
        ev = fnum(r['events'])
        if s == 'graphone':
            mb = go_rss.get(c)
            if not mb:
                return None, 'n/a'
            return max(0.0, mb * 1e6 / ev - 24), ''
        if s == 'rocksdb':
            return None, 'disk'         # LSM data lives in SST files, not in resident memory
        if s in ('stinger', 'sortledton'):
            return None, 'pairs'        # they keep one edge per pair, not the event multiset
        rss = fnum(r['rss_MB'])
        if rss <= 0 or ev <= 0 or 'DNF' in r.get('note', ''):
            return None, 'n/a' if rss <= 0 else 'did not finish'
        return rss * 1e6 / ev, ''
    grouped_bars(b, CORPORA, SYS_ORDER, bpe, log=False,
                 annotate=lambda y: f'{y:.0f}')
    b.set_ylabel('resident bytes per event')
    b.axhline(20, color=GREY, lw=0.6, ls=':')
    b.text(len(CORPORA) - 0.5, 21, '20 B packed event', ha='right', va='bottom',
           fontsize=5.8, color=GREY)
    panel_label(b, '(c) Memory')

    for ax in (a, c, b):
        ax.tick_params(axis='x', labelsize=6.3)
        for lbl in ax.get_xticklabels():
            lbl.set_rotation(12)
    fig.legend(handles=legend_handles(SYS_ORDER), ncol=7, frameon=False,
               fontsize=6.8, loc='lower center', bbox_to_anchor=(0.5, -0.04),
               handlelength=1.4, columnspacing=1.2)
    fig.tight_layout(rect=(0, 0.07, 1, 1), w_pad=1.2)
    fig.savefig(FIG + 'fig_ingest.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_ingest()
    print("wrote", FIG + "fig_ingest.pdf")
