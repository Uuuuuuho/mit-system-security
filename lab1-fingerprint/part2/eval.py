#!/usr/bin/env python3
"""eval.py - train a classifier on collected traces and report how well it fingerprints sites.
(Part 2, Exercise 2-2)

    python3 part2/eval.py traces/sweep.npz [--repeats 10] [--test-frac 0.3] [--model auto|rf|knn]

The script calls evaluate() --repeats times with different seeds (random stratified splits) and
prints one combined classification report plus the mean accuracy ("accuracy: 0.xxx").
Only numpy is required; scikit-learn's RandomForest is used when it is installed.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import fpdata  # noqa: E402


def preprocess(X, bin_windows=4):
    """Trace -> feature vector. A reasonable start, provided for you:
    normalize every trace by its own 95th percentile (~ "undisturbed" level, so 1.0 = idle) and
    average groups of `bin_windows` windows (less noise, fewer features).
    Optional: improve it (summary statistics, a spectrum for periodic sites ...). Discussion 2-3."""
    X = np.asarray(X, dtype=np.float64)
    Xn = X / np.maximum(np.percentile(X, 95, axis=1, keepdims=True), 1e-9)
    W = Xn.shape[1] // bin_windows * bin_windows
    return Xn[:, :W].reshape(len(Xn), -1, bin_windows).mean(axis=2)


def stratified_split(y, test_frac, rng):
    """Return index arrays (train, test). Every class must contribute ~test_frac of its traces
    (at least one, and at least one must stay in training) to the test set.
    rng is a numpy Generator (np.random.default_rng(seed)): use it for all randomness."""
    # TODO(Exercise 2-2)
    raise NotImplementedError("Exercise 2-2: stratified_split()")


def knn_predict(Xtr, ytr, Xte, k=1):
    """numpy-only k-nearest-neighbour classifier (Euclidean distance, majority vote).
    Returns an array of predicted labels, one per row of Xte."""
    # TODO(Exercise 2-2)
    raise NotImplementedError("Exercise 2-2: knn_predict()")


def evaluate(X, y, test_frac=0.3, seed=0, model="auto"):
    """One train/test experiment. Returns (y_true, y_pred) for the test traces.

    1. split with stratified_split(y, test_frac, np.random.default_rng(seed))
    2. features with preprocess(X)
    3. model == "rf" (or "auto" with fpdata.HAVE_SKLEARN): sklearn.ensemble.RandomForestClassifier
       (e.g. 200 trees, random_state=seed); otherwise knn_predict. Scale-sensitive models should
       be standardized with statistics computed on the TRAINING rows only.
    4. never let a test trace influence training
    """
    # TODO(Exercise 2-2)
    raise NotImplementedError("Exercise 2-2: evaluate()")


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
    try:
        for r in range(a.repeats):
            yt, yp = evaluate(X, y, a.test_frac, a.seed + r, a.model)
            yt_all.extend(yt)
            yp_all.extend(yp)
            accs.append(float(np.mean(np.asarray(yt) == np.asarray(yp))))
    except NotImplementedError as e:
        sys.exit(f"eval.py: not implemented yet: {e}")
    print(f"model: {model}, {a.repeats} random stratified splits, test fraction {a.test_frac}")
    print(fpdata.classification_report(yt_all, yp_all, names))
    print(f"accuracy: {np.mean(accs):.3f} (std over splits {np.std(accs):.3f}, chance {1 / len(names):.3f})")


if __name__ == "__main__":
    main()
