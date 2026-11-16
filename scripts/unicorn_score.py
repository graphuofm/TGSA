#!/usr/bin/env python3
"""Score test graphs against UNICORN models and report a continuous anomaly score
per graph, using the modeler's own Model/BestClusterGroup code
(crimson-unicorn/modeler, Python 3 copy in baselines/unicorn/modeler_py3).

UNICORN's decision rule (helper/profile.py test_single_graph): walk a model's
cluster evolution; a sketch whose Hamming distance to the current medoid exceeds
(threshold + num_stds * std) advances the evolution; if the next medoid still does
not fit, the graph does not fit this model. The graph is ABNORMAL iff it fits no
model. We keep that rule and additionally report, for each test graph, the
excess = min over models of max over sketches of (distance - threshold) along the
same walk, in units of the threshold's std. excess > 0 <=> ABNORMAL under the rule.

usage: unicorn_score.py --train-dir D --test-dir D [--metric mean|max] [--num-stds N] [--seed S] [--out TSV]
"""
import argparse, os, sys, random
import numpy as np
sys.path.insert(0, '/home/jding/Barry/baselines/unicorn/modeler_py3')
import warnings; warnings.filterwarnings('ignore')
from helper.profile import BestClusterGroup, Model
from scipy.spatial.distance import pdist, squareform, hamming


def load(fn):
    return np.array([[int(x) for x in l.split()] for l in open(fn) if l.strip()])


def build(train_files):
    models = []
    for tf in train_files:
        sk = load(tf)
        dists = squareform(pdist(sk, metric='hamming'))
        g = BestClusterGroup()
        g.optimize(arrs=sk, distance=lambda x, y: dists[x][y], max_cluster_num=6, num_trials=20, max_iterations=1000)
        m = Model(tf); m.construct(sk, dists, g); models.append(m)
    return models


def walk(sketches, model, metric, num_stds):
    """Returns (fits, max_excess_in_stds, fail_index)."""
    evo = model.get_evolution()
    if not evo:
        return None
    thr = model.get_mean_thresholds() if metric == 'mean' else model.get_max_thresholds()
    stds = model.get_stds()
    ei = 0; c = evo[ei]
    worst = -np.inf; fail = None
    for i, s in enumerate(sketches):
        d = hamming(s, model.get_medoids()[c])
        t = thr[c] + num_stds * stds[c]
        if d > t:
            if ei < len(evo) - 1:
                ei += 1; c = evo[ei]
                d = hamming(s, model.get_medoids()[c]); t = thr[c] + num_stds * stds[c]
                if d > t:
                    worst = max(worst, (d - thr[c]) / (stds[c] if stds[c] > 0 else 1e-9)); fail = i
                    return False, worst, fail
            else:
                worst = max(worst, (d - thr[c]) / (stds[c] if stds[c] > 0 else 1e-9)); fail = i
                return False, worst, fail
        worst = max(worst, (d - thr[c]) / (stds[c] if stds[c] > 0 else 1e-9))
    return True, worst, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--train-dir', required=True)
    ap.add_argument('--test-dir', required=True)
    ap.add_argument('--metric', default='mean', choices=['mean', 'max'])
    ap.add_argument('--num-stds', type=float, default=1.0)
    ap.add_argument('--seed', type=int, default=98765432)
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    random.seed(a.seed); np.random.seed(a.seed)
    train = sorted(os.path.join(a.train_dir, f) for f in os.listdir(a.train_dir) if f.startswith('sketch-'))
    test = sorted(os.path.join(a.test_dir, f) for f in os.listdir(a.test_dir) if f.startswith('sketch-'))
    models = build(train)
    out = open(a.out, 'w') if a.out else sys.stdout
    out.write('graph\tlabel\tverdict\tfitted_models\tmodels\texcess_stds\tfail_sketch\tnum_sketches\n')
    tp = fp = tn = fn = 0
    for tf in test:
        sk = load(tf)
        name = os.path.basename(tf)[len('sketch-'):-4]
        label = 'attack' if 'attack' in name else 'benign'
        fitted = 0; excess = []; fails = []
        for m in models:
            r = walk(sk, m, a.metric, a.num_stds)
            if r is None: continue
            fits, worst, fail = r
            fitted += fits; excess.append(worst); fails.append(fail)
        verdict = 'NORMAL' if fitted > 0 else 'ABNORMAL'
        if verdict == 'ABNORMAL': 
            if label == 'attack': tp += 1
            else: fp += 1
        else:
            if label == 'attack': fn += 1
            else: tn += 1
        fi = [f for f in fails if f is not None]
        out.write(f"{name}\t{label}\t{verdict}\t{fitted}\t{len(models)}\t{min(excess):.3f}\t{max(fi) if fi and fitted == 0 else ''}\t{len(sk)}\n")
    prec = tp / (tp + fp) if tp + fp else float('nan')
    rec = tp / (tp + fn) if tp + fn else float('nan')
    acc = (tp + tn) / max(1, tp + tn + fp + fn)
    out.write(f"# metric={a.metric} num_stds={a.num_stds} train_graphs={len(train)} tp={tp} fp={fp} tn={tn} fn={fn} precision={prec:.3f} recall={rec:.3f} accuracy={acc:.3f}\n")
    if a.out: out.close(); print(f"tp={tp} fp={fp} tn={tn} fn={fn} precision={prec:.3f} recall={rec:.3f} accuracy={acc:.3f}")


if __name__ == '__main__':
    main()
