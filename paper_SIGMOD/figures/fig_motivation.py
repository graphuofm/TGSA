#!/usr/bin/env python3
"""fig_motivation.pdf -- regenerate with: python3 fig_motivation.py.

Figure 1. The CADETS intrusion as the paper sees it: a chain of events in
time order, two analyst questions that are traversals along the time axis,
and a retention horizon deleting the oldest end of the same axis. Drawn,
not measured; every element is from the published ground-truth narrative.
The intent is written on the figure itself in three numbered statements,
so the reader does not have to infer it from the caption.
"""
from figstyle import *

def fig_motivation():
    fig, ax = plt.subplots(figsize=(3.34, 1.72))
    ax.set_xlim(0, 11); ax.set_ylim(-0.44, 1.22)
    ax.axis('off')
    G = PATH['qnbr']

    # (1) the stream: event ticks in arrival order along the time axis
    ax.annotate('', xy=(10.95, 0.0), xytext=(0.05, 0.0),
                arrowprops=dict(arrowstyle='-|>', lw=1.1, color='black'))
    ax.text(10.9, -0.13, 'time', ha='right', fontsize=6.5, style='italic')
    import random
    random.seed(7)
    for x in [random.uniform(0.3, 10.6) for _ in range(130)]:
        ax.plot([x, x], [0.02, 0.10], color=LIGHT, lw=0.45, zorder=1)
    ax.text(0.05, -0.265,
            r'$\bf{1}$ events arrive in time order, never change',
            fontsize=6.0, ha='left', va='bottom')

    # (3) retention: the oldest prefix is deleted
    ax.axvspan(0.05, 2.45, ymin=0.17, ymax=0.93, color='black', alpha=0.07,
               zorder=0)
    ax.annotate('', xy=(2.95, 0.98), xytext=(1.85, 0.98),
                arrowprops=dict(arrowstyle='-|>', lw=0.9, color=GREY))
    ax.text(1.25, 1.03, r'$\bf{3}$ retention' '\nhorizon',
            ha='center', fontsize=5.9, color=GREY, va='bottom',
            linespacing=0.95)

    # the attack chain: a time-respecting path through the stream
    chain = [(3.4, 'implant\ndrop'), (5.1, 'privilege\nescalation'),
             (6.9, 'C&C\ntraffic'), (8.6, 'process\ninjection'),
             (10.1, 'alert')]
    ys = [0.42, 0.62, 0.42, 0.62, 0.50]
    above = [True, False, True, False, True]
    for (x, lbl), y, up in zip(chain, ys, above):
        last = (lbl == 'alert')
        ax.scatter([x], [y], s=52 if last else 30, c=SYS['tgsr']['c'],
                   marker='*' if last else 'o', zorder=4,
                   edgecolors='white', linewidths=0.5)
        ax.plot([x, x], [0.02, y - 0.045], color=SYS['tgsr']['c'], lw=0.5,
                ls=(0, (1, 1)), zorder=2)
        ty = y + 0.115 if up else y - 0.245
        ax.text(x, ty, lbl, ha='center', fontsize=5.8, linespacing=0.95,
                zorder=5, bbox=dict(boxstyle='square,pad=0.05', fc='white',
                                    ec='none', alpha=0.75))
    for (x0, _), y0, (x1, _), y1 in zip(chain, ys, chain[1:], ys[1:]):
        ax.add_patch(FancyArrowPatch((x0 + 0.13, y0), (x1 - 0.13, y1),
                     arrowstyle='-|>', mutation_scale=7, lw=0.9,
                     color=SYS['tgsr']['c'], zorder=3,
                     connectionstyle='arc3,rad=-0.18'))

    # (2) the two questions: traversals along the time axis, both Q3
    ax.annotate('', xy=(3.6, 0.24), xytext=(9.9, 0.24),
                arrowprops=dict(arrowstyle='-|>', lw=0.9, color=G,
                                linestyle=(0, (3, 2))))
    ax.text(6.7, 0.135, 'Q3 backward: which earlier events caused the alert?',
            ha='center', fontsize=5.8, color=G)
    ax.annotate('', xy=(10.3, 0.90), xytext=(3.6, 0.90),
                arrowprops=dict(arrowstyle='-|>', lw=0.9, color=G))
    ax.text(7.0, 0.955, 'Q3 forward: what did the implant touch afterwards?',
            ha='center', fontsize=5.8, color=G)
    ax.text(0.05, -0.415,
            r'$\bf{2}$ the questions walk the time axis; $\bf{3}$ retention deletes its oldest end',
            fontsize=6.0, ha='left', va='bottom', color=G)
    fig.tight_layout(pad=0.2)
    fig.savefig(FIG + 'fig_motivation.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_motivation()
    print("wrote", FIG + "fig_motivation.pdf")
