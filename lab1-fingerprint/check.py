#!/usr/bin/env python3
"""check.py - grader for lab1-fingerprint (quick mode). Run through `make check`.

    python3 check.py --bin build --eval part2/eval.py                     (your code)
    python3 check.py --bin build/solution --eval solutions/part2/eval.py  (reference)

Options: --layout shared|split|none (default shared), --traces N, --duration S, --period MS,
         --seed N, --keep DIR (keep the quick datasets), --only 1,2,3 (run some parts only).
Exit code 0 only if every test passes.
"""
import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))
import fpdata  # noqa: E402

RESULTS = []  # (part, name, ok, points, detail)


def record(part, name, ok, points, detail=""):
    RESULTS.append((part, name, ok, points, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {part} {name}" + (f": {detail}" if detail else ""), flush=True)
    return ok


def run(cmd, timeout=120, **kw):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -999, "", f"timeout after {timeout}s"
    except OSError as e:
        return -998, "", str(e)


def collector_counts(binary, buf, period=10, duration=0.3):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "t.txt")
        rc, _, err = run([binary, "-P", str(period), "-T", str(duration), "-b", buf, "-o", out], timeout=30)
        if rc != 0:
            raise RuntimeError(f"{os.path.basename(binary)} exited with {rc}: {err.strip()[:200]}")
        hdr, counts = fpdata.read_trace(out)
        return hdr, counts


# ----------------------------------------------------------------------------------------- Part 1
def part1(a):
    print("Part 1: timer warm-up")
    timer = os.path.join(a.bin, "timer")
    rc, out, err = run([timer, "--selftest"], timeout=30)
    record("1", "selftest (coarsen, timed_chase, single_shot_accuracy)", rc == 0 and "selftest: PASS" in out, 1,
           (out + err).strip().replace("\n", " | ")[:300] if rc != 0 else "")
    rc, out, err = run([timer, "--quick"], timeout=120)
    m = re.search(r"measurable: native=(-?\d+) coarse_0\.1ms=(-?\d+) coarse_1ms=(-?\d+)", out)
    if rc != 0 or not m:
        record("1", "quick sweep N = 1 .. 1e6", False, 1, f"no 'measurable:' line (exit {rc}) {err.strip()[:200]}")
        return
    nat, c01, c1 = (int(v) for v in m.groups())
    ok = nat > 0 and c01 > 0 and c1 > 0 and nat <= c01 <= c1 and c1 > nat
    record("1", "quick sweep N = 1 .. 1e6", ok, 1,
           f"smallest measurable N: native={nat} 0.1ms={c01} 1ms={c1}"
           + ("" if ok else " (expected all found and native <= 0.1ms <= 1ms, 1ms > native)"))


# ----------------------------------------------------------------------------------------- Part 2
def load_eval(path):
    spec = importlib.util.spec_from_file_location("student_eval", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def synthetic_dataset(seed):
    """4 easy classes x 12 traces x 120 windows with noise and random shifts."""
    rng = np.random.default_rng(seed)
    X, y = [], []
    t = np.arange(120)
    for c in range(4):
        for _ in range(12):
            s = rng.integers(0, 6)
            if c == 0:
                base = np.ones(120)
            elif c == 1:
                base = 1.0 - 0.5 * (((t + s) // 10) % 2)
            elif c == 2:
                base = np.where(t + s < 60, 0.5, 1.0)
            else:
                base = np.where(((t + s) % 30) < 4, 0.3, 1.0)
            X.append(100 * base + rng.normal(0, 6, 120))
            y.append(c)
    return np.array(X), np.array(y)


def collect_and_eval(a, collector, tag, traces, eval_py):
    out = os.path.join(a.keep or a.tmp, f"quick_{tag}.npz")
    cmd = [sys.executable, os.path.join(HERE, "tools", "collect.py"), "--collector", collector,
           "--traces", str(traces), "--duration", str(a.duration), "--period", str(a.period),
           "--layout", a.layout, "--out", out, "--quiet"]
    if a.seed is not None:
        cmd += ["--seed", str(a.seed)]
    t0 = time.time()
    rc, so, se = run(cmd, timeout=600)
    if rc != 0:
        return None, f"collect.py failed ({rc}): {(so + se).strip()[-300:]}"
    rc, so, se = run([sys.executable, eval_py, out, "--repeats", "10"], timeout=300)
    m = re.search(r"accuracy: ([0-9.]+)", so)
    if rc != 0 or not m:
        return None, f"eval.py failed ({rc}): {(so + se).strip()[-300:]}"
    return float(m.group(1)), f"{traces} traces/site x {a.duration}s, P={a.period}ms, layout={a.layout}, " \
                              f"{time.time() - t0:.0f}s"


def part2(a):
    print("Part 2: sweep counting")
    collector = os.path.join(a.bin, "collector")
    ok_col = False
    try:
        _, small = collector_counts(collector, "1M")
        _, large = collector_counts(collector, "8M")
        # medians: a window can legitimately be 0 when another process preempts us for >= 10 ms
        ms, ml = np.median(small), np.median(large)
        if ms <= 0 or ml <= 0:
            record("2", "collector sweeps the buffer", False, 1,
                   f"median sweeps per window is 0 (1M: {ms:.0f}, 8M: {ml:.0f}); is measure_window() implemented?")
        else:
            ratio = ms / ml
            ok_col = record("2", "collector sweeps the buffer", ratio >= 3.0, 1,
                            f"median sweeps/10ms: 1 MiB buffer {ms:.0f}, 8 MiB buffer {ml:.0f} "
                            f"(ratio {ratio:.1f}, need >= 3: work must scale with buffer size)")
    except Exception as e:  # noqa: BLE001
        record("2", "collector sweeps the buffer", False, 1, str(e))

    ok_eval = False
    try:
        mod = load_eval(a.eval)
        models = ["knn"] + (["rf"] if fpdata.HAVE_SKLEARN else [])
        accs = {}
        for model in models:
            vals = []
            for seed in range(3):
                X, y = synthetic_dataset(100 + seed)
                yt, yp = mod.evaluate(X, y, 0.3, seed, model)
                yt, yp = np.asarray(yt), np.asarray(yp)
                if len(yt) != len(yp) or not (8 <= len(yt) <= 24):
                    raise RuntimeError(f"evaluate() returned {len(yt)} true / {len(yp)} predicted labels for 48 "
                                       "traces with test_frac=0.3 (expected ~16 each)")
                per_class = np.bincount(yt, minlength=4)
                if per_class.min() == 0 or per_class.max() - per_class.min() > 1:
                    raise RuntimeError(f"test set is not stratified: test traces per class {per_class.tolist()} "
                                       "(12 traces per class, so every class should contribute equally)")
                vals.append(float(np.mean(yt == yp)))
            accs[model] = float(np.mean(vals))
        ok_eval = all(v >= 0.9 for v in accs.values())
        record("2", "evaluate() on a synthetic dataset", ok_eval, 1,
               ", ".join(f"{k} accuracy {v:.2f}" for k, v in accs.items()) + " (need >= 0.90 each)")
    except NotImplementedError as e:
        record("2", "evaluate() on a synthetic dataset", False, 1, f"not implemented: {e}")
    except Exception as e:  # noqa: BLE001
        record("2", "evaluate() on a synthetic dataset", False, 1, f"{type(e).__name__}: {e}")

    if not (ok_col and ok_eval):
        record("2", f"end-to-end fingerprinting accuracy >= {a.threshold}", False, 2, "skipped: fix the tests above first")
        return None
    acc, detail = collect_and_eval(a, collector, "sweep", a.traces, a.eval)
    record("2", f"end-to-end fingerprinting accuracy >= {a.threshold}", acc is not None and acc >= a.threshold, 2,
           (f"accuracy {acc:.3f} (chance 0.125); " if acc is not None else "") + detail)
    return acc


# ----------------------------------------------------------------------------------------- Part 3
def part3(a, sweep_acc):
    print("Part 3: root cause (counter collector, no memory accesses)")
    counter = os.path.join(a.bin, "collector_counter")
    ok = False
    try:
        _, small = collector_counts(counter, "1M")
        _, large = collector_counts(counter, "8M")
        ms, ml = np.median(small), np.median(large)
        if ms <= 0 or ml <= 0:
            record("3", "counter collector ignores the buffer", False, 1,
                   "median blocks per window is 0; is measure_window() implemented?")
        else:
            ratio = ms / ml
            ok = record("3", "counter collector ignores the buffer", 0.5 <= ratio <= 2.0, 1,
                        f"median blocks/10ms: -b 1M {ms:.0f}, -b 8M {ml:.0f} (ratio {ratio:.2f}, "
                        "need 0.5..2: no work may depend on the buffer)")
    except Exception as e:  # noqa: BLE001
        record("3", "counter collector ignores the buffer", False, 1, str(e))
    if not ok:
        record("3", "counter collector end-to-end run", False, 1, "skipped: fix the test above first")
        return
    acc, detail = collect_and_eval(a, counter, "counter", a.traces3, a.eval)
    cmp = f"; Part 2 sweep accuracy was {sweep_acc:.3f}" if sweep_acc is not None else ""
    record("3", "counter collector end-to-end run", acc is not None, 1,
           (f"accuracy {acc:.3f} (reported, no threshold){cmp}; " if acc is not None else "") + detail)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build")
    ap.add_argument("--eval", default="part2/eval.py")
    ap.add_argument("--layout", default=os.environ.get("LAYOUT", "shared"), choices=["shared", "split", "none"])
    ap.add_argument("--traces", type=int, default=6, help="quick mode traces per site (Part 2)")
    ap.add_argument("--traces3", type=int, default=4, help="quick mode traces per site (Part 3)")
    ap.add_argument("--duration", type=float, default=1.0)
    ap.add_argument("--period", type=float, default=10.0)
    ap.add_argument("--threshold", type=float, default=0.60)
    ap.add_argument("--seed", type=int, default=int(os.environ["SEED"]) if os.environ.get("SEED") else None,
                    help="site jitter / order seed (default: $SEED or time)")
    ap.add_argument("--keep", default=None, help="directory to keep the quick datasets in")
    ap.add_argument("--only", default="1,2,3")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    a.eval = os.path.abspath(a.eval)
    if a.keep:
        os.makedirs(a.keep, exist_ok=True)
    t0 = time.time()
    print(f"lab1-fingerprint check: binaries in {a.bin}, eval {os.path.relpath(a.eval, HERE)}, "
          f"{os.cpu_count()} CPUs, sklearn={'yes' if fpdata.HAVE_SKLEARN else 'no (numpy k-NN)'}")
    parts = set(a.only.split(","))
    with tempfile.TemporaryDirectory(prefix="fpcheck-") as tmp:
        a.tmp = tmp
        if "1" in parts:
            part1(a)
        sweep_acc = part2(a) if "2" in parts else None
        if "3" in parts:
            part3(a, sweep_acc)
    total = sum(p for *_, p, _ in RESULTS)
    got = sum(p for _, _, ok, p, _ in RESULTS if ok)
    print()
    for part in sorted({r[0] for r in RESULTS}):
        rs = [r for r in RESULTS if r[0] == part]
        print(f"Part {part}: {'PASS' if all(r[2] for r in rs) else 'FAIL'}  "
              f"({sum(r[3] for r in rs if r[2])}/{sum(r[3] for r in rs)})")
    print(f"score: {got}/{total}   ({time.time() - t0:.0f}s)")
    sys.exit(0 if got == total else 1)


if __name__ == "__main__":
    main()
