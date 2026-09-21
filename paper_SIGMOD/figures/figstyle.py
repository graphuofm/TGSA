#!/usr/bin/env python3
"""Shared style for the SIGMOD figures.

Each fig_*.pdf in this directory has a same-named fig_*.py next to it;
running `python3 fig_X.py` regenerates fig_X.pdf in place. This module
holds everything they share: palette, rcParams, CSV loaders, name maps.

Same accessibility discipline as the FAST set: Okabe-Ito palette, every
series redundantly encoded (colour AND marker AND linestyle; bars colour AND
hatch), no text below ~6.5pt at final size.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch, FancyArrowPatch

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, '..', '..'))
R = os.path.join(_REPO, 'results') + '/'
FIG = _HERE + '/'

plt.rcParams.update({
    'font.size': 8, 'font.family': 'serif',
    'font.serif': ['DejaVu Serif'],
    'axes.spines.top': False, 'axes.spines.right': False,
    'axes.labelsize': 8, 'axes.titlesize': 8.5,
    'xtick.labelsize': 7.5, 'ytick.labelsize': 7.5,
    'legend.fontsize': 7.2, 'figure.dpi': 400,
    'axes.grid': True, 'grid.alpha': 0.25, 'grid.linewidth': 0.4,
    'axes.axisbelow': True, 'lines.linewidth': 1.4, 'lines.markersize': 4,
})

C = dict(sys='#000000', tcsr='#0072B2', ucsr='#E69F00', csr='#009E73',
         duckdb='#D55E00', tva='#CC79A7', lg='#E69F00', go='#56B4E9',
         rocks='#D55E00', grey='#555555')

def med(v):
    v = sorted(v); n = len(v)
    return v[n//2] if n % 2 else 0.5*(v[n//2-1]+v[n//2])

def load(path):
    return list(csv.DictReader(open(R + path)))

SHORT = {'sx-askubuntu': 'askubuntu', 'wiki-talk-temporal': 'wiki-talk',
         'cit-Patents': 'cit-Patents', 'tc-cadets-e3': 'CADETS',
         'tc-theia-e3': 'THEIA', 'lanl-auth': 'LANL',
         'sx-mathoverflow': 'mathoverflow', 'sx-superuser': 'superuser',
         'email-Eu-core-temporal': 'email-Eu', 'CollegeMsg': 'CollegeMsg',
         'act-mooc': 'act-mooc', 'sx-stackoverflow': 'stackoverflow',
         'soc-LiveJournal1': 'LiveJournal', 'bitcoin-temporal': 'bitcoin'}

STYLE = {
    'barry':  dict(c='#000000', m='o', ls='-',   lbl='TGSR'),
    'tcsr':   dict(c='#0072B2', m='s', ls='--',  lbl='sorted CSR (static)'),
    'ucsr':   dict(c='#E69F00', m='^', ls='-.',  lbl='arrival CSR (ingesting)'),
    'csr':    dict(c='#009E73', m='D', ls=(0,(1,1)), lbl='topology CSR (optimum)'),
    'duckdb': dict(c='#D55E00', m='v', ls=(0,(3,1,1,1)), lbl='DuckDB (columnar)'),
}

