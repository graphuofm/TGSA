#!/usr/bin/env python3
"""fig_deploy.pdf -- regenerate with: python3 fig_deploy.py (edits welcome).

Three deployment axes the engine is measured on: what each docked module
costs (E25), what happens when writers and readers run at once against
the same three systems (E26), and whether the design decomposes across
shards (E27).
"""
from figstyle import *

def fig_deploy():
    fig, (a, b, c) = plt.subplots(1, 3, figsize=(7.0, 1.78),
                                  gridspec_kw={'width_ratios': [1.15, 1.2, 1]})

    # ---------- (a) dock ladder: medians per rung, 3 corpora ----------
    rows = load('e25_dock/e25.csv')
    g = defaultdict(list)
    for r in rows:
        g[(r['dataset'], r['rung'])].append(float(r['ingest_Meps']))
    rungs = ['core', '+vindex', '+reverse', '+props16', '+wal', 'full']
    corp = [('wiki-talk-temporal', '#000000', 'o', '-'),
            ('cit-Patents', '#0072B2', 's', '--'),
            ('tc-cadets-e3', '#E69F00', '^', '-.')]
    for ds, col, mk, ls in corp:
        ys = [med(g[(ds, ru)]) for ru in rungs]
        a.plot(range(len(rungs)), ys, marker=mk, ls=ls, color=col,
               ms=3.6, lw=1.15, label=SHORT[ds], zorder=3)
    a.set_yscale('log')
    a.set_xticks(range(len(rungs)))
    a.set_xticklabels(['core', '+vidx', '+rev', '+prop', '+wal', 'full'],
                      fontsize=6.0, rotation=28, ha='right',
                      rotation_mode='anchor')
    a.set_ylabel('M events/s')
    a.set_ylim(top=900)
    a.legend(frameon=False, fontsize=6.0, loc='upper right', handlelength=1.7,
             borderpad=0.1, labelspacing=0.2)
    a.set_title('(a) Price of each docked module', loc='left', fontsize=8)

    # ---------- (b) concurrent head-to-head at 8 writers : 4 readers ----
    rows = load('e26_concurrent/e26.csv')
    corpora = ['sx-askubuntu', 'wiki-talk-temporal', 'cit-Patents', 'tc-cadets-e3']
    sysmap = [('tgsr', 'TGSR', '#000000', ''),
              ('graphone', 'GraphOne', C['go'], '\\\\\\'),
              ('livegraph_sharded', 'LiveGraph', C['lg'], '///')]
    vals = defaultdict(dict)
    for r in rows:
        if r['writers'] == '8' and r['readers'] == '4':
            vals[r['system']][r['dataset']] = float(r['ingest_Meps'])
    W = 0.26
    for j, (key, lbl, col, hat) in enumerate(sysmap):
        xs = [i + (j - 1) * W for i in range(len(corpora))]
        ys = [vals[key].get(d, float('nan')) for d in corpora]
        b.bar(xs, ys, width=W, color=col, hatch=hat, edgecolor='white',
              linewidth=0.5, label=lbl, zorder=3)
    b.set_yscale('log'); b.set_ylim(1, 700)
    b.set_ylabel('M events/s under\n4 concurrent readers')
    b.set_xticks(range(len(corpora)))
    b.set_xticklabels([SHORT[d] for d in corpora], fontsize=6.0,
                      rotation=24, ha='right', rotation_mode='anchor')
    b.legend(frameon=False, fontsize=6.0, ncol=3, loc='upper center',
             handlelength=1.0, handletextpad=0.3, columnspacing=0.7,
             borderpad=0.1)
    b.set_title('(b) Writers and readers at once', loc='left', fontsize=8)

    # ---------- (c) scale-out: speedup vs shards ----------
    rows = load('e27_shard/e27.csv')
    ser = defaultdict(dict)
    for r in rows:
        if r['dataset'] != 'cit-Patents':
            continue
        ser[(r['partition'], r['ingest_mode'])][int(r['shards'])] = \
            float(r['speedup_vs_1'])
    style = [(('by_src', 'live'),   'entity shard, live',  '#000000', 'o', '-'),
             (('by_time', 'live'),  'time shard, live',    '#D55E00', 'x', ':'),
             (('by_src', 'replay'), 'entity shard, replay', '#0072B2', 's', '--'),
             (('by_time', 'replay'), 'time shard, replay', '#009E73', 'D', (0, (1, 1)))]
    for key, lbl, col, mk, ls in style:
        d = ser[key]
        xs = sorted(d)
        c.plot(xs, [d[x] for x in xs], marker=mk, ls=ls, color=col,
               ms=3.4, lw=1.1, label=lbl, mfc=col if mk != 'x' else 'none',
               zorder=3)
    c.axhline(1.0, color='#888888', lw=0.9, ls=(0, (1, 2)), zorder=2)
    c.set_xscale('log', base=2); c.set_xticks([1, 2, 4, 8])
    c.set_xticklabels(['1', '2', '4', '8'])
    c.set_xlabel('shards (16 threads split among them)')
    c.set_ylabel('ingest, $\\times$ one shard')
    c.set_ylim(0.15, 3.9)
    c.legend(frameon=False, fontsize=5.4, loc='upper center', ncol=2,
             handlelength=1.4, handletextpad=0.3, columnspacing=0.6,
             borderpad=0.1, labelspacing=0.18)
    c.set_title('(c) Scale-out of the design', loc='left', fontsize=8)

    fig.tight_layout(pad=0.35, w_pad=1.25)
    fig.savefig(FIG + 'fig_deploy.pdf'); plt.close(fig)

if __name__ == "__main__":
    fig_deploy()
    print("wrote", FIG + "fig_deploy.pdf")
