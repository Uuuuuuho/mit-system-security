#!/usr/bin/env python3
"""eval.py - train a classifier on collected traces and report how well it fingerprints sites.
REFERENCE SOLUTION.

    python3 part2/eval.py traces/sweep.npz [--repeats 10] [--test-frac 0.3] [--model auto|rf|knn]

The script calls evaluate() --repeats times with different seeds (random stratified splits) and
prints one combined classification report plus the mean accuracy ("accuracy: 0.xxx").
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import fpdata  # noqa: E402


def preprocess(X, bin_windows=4, n_freq=30):
    """Trace -> feature vector.

    1. Normalize every trace by its own 95th percentile. That level is roughly "the collector
       ran undisturbed", so 1.0 = idle and 0.5 = lost half the window. This removes machine-
       dependent scale (buffer size, clock speed) without looking at other traces.
    2. Average groups of `bin_windows` windows: less noise, fewer features (few training traces!).
    3. Append summary statistics (mean level, spread, percentiles, burstiness).
    4. Append the normalized magnitude spectrum: periodic sites (video frames, polling) have
       peaks there even when their phase differs from run to run.
    """
    X = np.asarray(X, dtype=np.float64)
    ref = np.percentile(X, 95, axis=1, keepdims=True)
    Xn = X / np.maximum(ref, 1e-9)
    W = Xn.shape[1] // bin_windows * bin_windows
    binned = Xn[:, :W].reshape(len(Xn), -1, bin_windows).mean(axis=2)
    stats = np.column_stack([Xn.mean(1), Xn.std(1), np.percentile(Xn, 10, axis=1), np.median(Xn, axis=1),
                             np.percentile(Xn, 90, axis=1), np.abs(np.diff(Xn, axis=1)).mean(1)])
    spec = np.abs(np.fft.rfft(Xn - Xn.mean(axis=1, keepdims=True), axis=1))[:, 1:n_freq + 1]
    spec = spec / (spec.sum(axis=1, keepdims=True) + 1e-9)
    return np.hstack([binned, stats, spec])


def stratified_split(y, test_frac, rng):
    """Index arrays (train, test) with ~test_frac of every class in the test set (>= 1 each)."""
    train, test = [], []
    for c in np.unique(y):
        idx = rng.permutation(np.flatnonzero(y == c))
        n_test = min(len(idx) - 1, max(1, int(round(test_frac * len(idx)))))
        test.extend(idx[:n_test])
        train.extend(idx[n_test:])
    return np.array(train), np.array(test)


def knn_predict(Xtr, ytr, Xte, k=1):
    """Plain numpy k-nearest-neighbours (Euclidean), majority vote."""
    d = ((Xte[:, None, :] - Xtr[None, :, :]) ** 2).sum(axis=2)
    nn = np.argsort(d, axis=1)[:, :k]
    votes = ytr[nn]
    return np.array([np.bincount(v).argmax() for v in votes])


def evaluate(X, y, test_frac=0.3, seed=0, model="auto"):
    """One train/test experiment. Returns (y_true, y_pred) for the test traces.

    model: "rf" = RandomForest (scikit-learn), "knn" = numpy k-NN, "auto" = rf if available."""
    rng = np.random.default_rng(seed)
    tr, te = stratified_split(y, test_frac, rng)
    Xp = preprocess(X)
    if model == "rf" or (model == "auto" and fpdata.HAVE_SKLEARN):
        from sklearn.ensemble import RandomForestClassifier
        clf = RandomForestClassifier(n_estimators=200, random_state=seed, n_jobs=1)
        clf.fit(Xp[tr], y[tr])
        pred = clf.predict(Xp[te])
    else:
        # k-NN is scale sensitive: z-score every feature with TRAINING statistics only.
        mu, sd = Xp[tr].mean(axis=0), Xp[tr].std(axis=0) + 1e-9
        pred = knn_predict((Xp[tr] - mu) / sd, y[tr], (Xp[te] - mu) / sd, k=1)
    return y[te], pred


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dataset")
    ap.add_argument("--repeats", type=int, default=10)
    ap.add_argument("--test-frac", type=float, default=0.3)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--model", choices=["auto", "rf", "knn"], default="auto")
    a = ap.parse_args()

    X, y, names, meta = fpdata.load_dataset(a.dataset)
    print(f"dataset: {len(y)} traces, {X.shape[1]} windows, {len(names)} sites; "
          f"collector={meta.get('collector')} P={meta.get('P_ms')}ms T={meta.get('T_s')}s layout={meta.get('layout')}")
    model = a.model if a.model != "auto" else ("rf" if fpdata.HAVE_SKLEARN else "knn")
    yt_all, yp_all, accs = [], [], []
    for r in range(a.repeats):
        yt, yp = evaluate(X, y, a.test_frac, a.seed + r, a.model)
        yt_all.extend(yt)
        yp_all.extend(yp)
        accs.append(float(np.mean(np.asarray(yt) == np.asarray(yp))))
    print(f"model: {model}, {a.repeats} random stratified splits, test fraction {a.test_frac}")
    print(fpdata.classification_report(yt_all, yp_all, names))
    print(f"accuracy: {np.mean(accs):.3f} (std over splits {np.std(accs):.3f}, chance {1 / len(names):.3f})")


if __name__ == "__main__":
    main()
