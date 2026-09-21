#!/usr/bin/env python3
"""fig_motivation.pdf -- regenerate with: python3 fig_motivation.py (edits welcome)."""
from figstyle import *

# =====================================================================
# Fig. 1 (motivation): the CADETS attack chain on a timeline, retention
# closing in from the left. Drawn, not measured; every element is from the
# published ground truth narrative.
# =====================================================================
def fig_motivation():
    fig, ax = plt.subplots(figsize=(3.34, 1.42))
    ax.set_xlim(0, 11); ax.set_ylim(-0.15, 1.18)
    ax.axis('off')

    # timeline arrow
    ax.annotate('', xy=(10.9, 0.0), xytext=(0.1, 0.0),
                arrowprops=dict(arrowstyle='-|>', lw=1.1, color='#000000'))
    ax.text(10.85, -0.14, 'time (11 days)', ha='right', fontsize=6.3,
            style='italic')

    # retention horizon sweeping from the left
    ax.axvspan(0.1, 2.6, ymin=0.08, ymax=0.98, color='#000000', alpha=0.08,
               zorder=0)
    ax.annotate('', xy=(3.15, 0.94), xytext=(2.0, 0.94),
                arrowprops=dict(arrowstyle='-|>', lw=0.9, color='#777777'))
    ax.text(1.35, 1.035, 'retention horizon\n(evidence deleted)', ha='center',
            fontsize=5.9, color='#555555', style='italic')

    # background event ticks: the stream the chain hides in
    import random
    random.seed(7)
    for x in [random.uniform(0.3, 10.6) for _ in range(130)]:
        ax.plot([x, x], [0.02, 0.10], color='#aaaaaa', lw=0.45, zorder=1)

    # the attack chain: time-respecting path. Labels alternate above/below
    # their node so none can touch the query arrows drawn at 0.24 and 0.86.
    chain = [(3.4, 'implant\ndrop'), (5.1, 'privilege\nescalation'),
             (6.9, 'C\\&C\ntraffic'), (8.6, 'process\ninjection'),
             (10.1, 'alert')]
    ys = [0.42, 0.62, 0.42, 0.62, 0.50]
    above = [True, False, True, False, True]
    for (x, lbl), y, up in zip(chain, ys, above):
        last = (lbl == 'alert')
        ax.scatter([x], [y], s=46 if last else 30,
                   c='#D55E00' if last else '#0072B2',
                   marker='*' if last else 'o', zorder=4,
                   edgecolors='white', linewidths=0.5)
        ax.plot([x, x], [0.02, y - 0.045], color='#0072B2', lw=0.5,
                ls=(0, (1, 1)), zorder=2)
        ty = y + 0.115 if up else y - 0.245
        ax.text(x, ty, lbl.replace('\\&', '&'), ha='center', fontsize=5.8,
                linespacing=0.95, zorder=5,
                bbox=dict(boxstyle='square,pad=0.05', fc='white', ec='none',
                          alpha=0.75))
    for (x0, _), y0, (x1, _), y1 in zip(chain, ys, chain[1:], ys[1:]):
        ax.add_patch(FancyArrowPatch((x0 + 0.13, y0), (x1 - 0.13, y1),
                     arrowstyle='-|>', mutation_scale=7, lw=0.9,
                     color='#0072B2', zorder=3,
                     connectionstyle='arc3,rad=-0.18'))

    # the two queries
    ax.annotate('', xy=(3.6, 0.24), xytext=(9.9, 0.24),
                arrowprops=dict(arrowstyle='-|>', lw=0.8, color='#009E73',
                                linestyle=(0, (3, 2))))
    ax.text(6.7, 0.145, 'backward: what caused the alert?', ha='center',
            fontsize=5.9, color='#007a58')
    ax.annotate('', xy=(10.3, 0.90), xytext=(3.6, 0.90),
                arrowprops=dict(arrowstyle='-|>', lw=0.8, color='#D55E00',
                                linestyle=(0, (3, 2))))
    ax.text(7.2, 0.955, 'forward: what did the implant touch?', ha='center',
            fontsize=5.9, color='#b34700')
    fig.tight_layout(pad=0.25)
    fig.savefig(FIG + 'fig_motivation.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_motivation()
    print("wrote", FIG + "fig_motivation.pdf")
