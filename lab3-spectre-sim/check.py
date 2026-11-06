#!/usr/bin/env python3
"""Grader for lab3-spectre-sim.

    python3 check.py                     grade src/ (your code)
    python3 check.py --impl solutions    grade the reference solution
    python3 check.py --parts 2,4 --seed 7 -v

Secrets and noise are randomized per run (SEED env var or --seed to repeat a run).
Exit status is 0 only if every graded part passes.
"""

import argparse
import importlib.util
import math
import os
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from specsim import Machine, BudgetExceeded, ProgramFault, AsmError   # noqa: E402
from specsim.bench import run_suite, BENCHMARKS                       # noqa: E402
from specsim.selftest import TESTS as SIM_TESTS                       # noqa: E402

# per-byte instruction budgets (generous: the reference uses a fraction of these)
BUDGET_P1 = 200_000
BUDGET_P2 = 400_000
BUDGET_P3 = 400_000
BUDGET_P4 = 120_000
P4_BYTES = range(5, 15)        # secret bytes used for the defense evaluation (skip "FLAG{")
# A hardware defense that simply delays or hides *every* speculative load also
# blocks the attack, but costs ~35-55% here; the intended designs cost far less.
OVERHEAD_LIMIT = {"invisispec": 0.30, "stt": 0.30}

POINTS = {"1-1": 5, "1-2": 15, "2": 25, "3-1": 5, "3-2": 20,
          "4-fence": 7.5, "4-slh": 7.5, "4-invisispec": 7.5, "4-stt": 7.5}


class Grader:
    def __init__(self, impl, seed, verbose, attack_impl):
        self.impl = impl
        self.attack_impl = attack_impl or impl
        self.seed = seed
        self.verbose = verbose
        self.results = {}     # item -> (passed, points)
        self._mods = {}

    # -------------------------------------------------------------- utilities
    def module(self, name, impl=None):
        impl = impl or self.impl
        key = (impl, name)
        if key not in self._mods:
            d = os.path.join(HERE, impl)
            if d not in sys.path:
                sys.path.insert(0, d)
            spec = importlib.util.spec_from_file_location(f"{impl}_{name}", os.path.join(d, name + ".py"))
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            self._mods[key] = mod
        return self._mods[key]

    def record(self, item, passed, msg):
        pts = POINTS[item] if passed else 0
        self.results[item] = (passed, pts)
        print(f"[{item:>12s}] {'PASS' if passed else 'FAIL'} ({pts:g}/{POINTS[item]:g})  {msg}")

    def note(self, msg):
        print(f"{'':16s}{msg}")

    def leak(self, m, fn, indices, budget):
        """Call fn(m, i) for each index under a per-byte instruction budget.
        Returns the list of guesses (None for a failed byte)."""
        out = []
        for i in indices:
            m.budget = budget
            try:
                g = fn(m, i)
                out.append(int(g) & 0xFF if g is not None else None)
            except BudgetExceeded:
                out.append(None)
            finally:
                m.budget = None
        return out

    @staticmethod
    def show(bs):
        return "".join(chr(b) if b is not None and 32 <= b < 127 else "." for b in bs)

    def fail(self, items, e):
        """Record failure of `items` because student code raised `e`."""
        kind = type(e).__name__
        first = str(e).splitlines()[0] if str(e) else ""
        for it in items:
            if it not in self.results:
                self.record(it, False, f"{kind}: {first}")
        if isinstance(e, (ProgramFault, AsmError)):
            print("\n".join("    " + ln for ln in str(e).splitlines()))
        else:
            traceback.print_exception(type(e), e, e.__traceback__, limit=None if self.verbose else -3)

    def guard(self, items, fn):
        """Run one grading step; any exception fails all its items."""
        try:
            fn()
        except Exception as e:     # student code may raise anything
            self.fail(items, e)

    # -------------------------------------------------------------- parts
    def selftest(self):
        bad = []
        for t in SIM_TESTS:
            try:
                t()
            except Exception as e:
                bad.append(f"{t.__name__}: {e}")
        print(f"[{'sim':>12s}] {'ok' if not bad else 'BROKEN'}  simulator selftest")
        for b in bad:
            self.note(b)
        return not bad

    def part1(self):
        p1 = self.module("part1_flush_reload")
        m = Machine(seed=self.seed + 1)

        def calib():
            thr = p1.calibrate(m)
            ok = isinstance(thr, (int, float)) and 20 <= thr <= 135
            self.record("1-1", ok, f"threshold = {thr}  (must separate hits <~20 from misses >~140)")
        self.guard(["1-1"], calib)

        def leak():
            secret = m._secret(1)
            t0 = time.time()
            got = self.leak(m, p1.leak_byte, range(len(secret)), BUDGET_P1)
            n = sum(g == s for g, s in zip(got, secret))
            self.record("1-2", n == len(secret),
                        f"Flush+Reload leaked {n}/{len(secret)} bytes (need all)  {time.time() - t0:.1f}s")
            self.note(f"leaked : {self.show(got)}")
            if self.verbose or n != len(secret):
                self.note(f"secret : {secret.decode()}")
        self.guard(["1-2"], leak)

    def part2(self):
        p2 = self.module("part2_spectre_v1")

        def leak():
            m = Machine(seed=self.seed + 2)
            secret = m._secret(2)
            t0 = time.time()
            got = self.leak(m, p2.leak_byte, range(len(secret)), BUDGET_P2)
            n = sum(g == s for g, s in zip(got, secret))
            self.record("2", n >= 0.9 * len(secret),
                        f"Spectre v1 leaked {n}/{len(secret)} bytes ({100 * n / len(secret):.0f}%, "
                        f"need >= 90%)  {time.time() - t0:.1f}s")
            self.note(f"leaked : {self.show(got)}")
            if self.verbose or n < len(secret):
                self.note(f"secret : {secret.decode()}")
        self.guard(["2"], leak)

    def part3(self):
        p3 = self.module("part3_short_window")
        m = Machine(seed=self.seed + 3)

        def evset():
            target = m.ksyms["k_v1h_limit"]
            ev = list(p3.eviction_set(m, target))
            if not ev:
                self.record("3-1", False, "eviction_set() returned nothing")
                return
            if len(ev) > 256 or not all(isinstance(a, int) and m.USER_BASE <= a < m.USER_END - 8
                                        for a in ev):
                self.record("3-1", False, "eviction set must be <= 256 user-memory addresses (ints)")
                return
            src = (".data\n.align 64\nlist: .zero %d\n.text\n_start:\n la s0, list\n li s1, %d\n"
                   "loop:\n ld t0, 0(s0)\n lb t1, 0(t0)\n addi s0, s0, 8\n addi s1, s1, -1\n"
                   " bnez s1, loop\n halt\n" % (8 * len(ev), len(ev)))
            gm = Machine(seed=self.seed + 33)
            prog = gm.load(src)
            gm.write_u64s(prog["list"], ev)
            evicted = 0
            for _ in range(5):
                gm._cpu.caches.access(target, gm.now())    # grader: put the bound in L1
                gm.run(prog)
                evicted += gm._cpu.caches.level_of(target) == 3
            self.record("3-1", evicted == 5,
                        f"{len(ev)} addresses; touching them evicted k_v1h_limit in {evicted}/5 trials")
        self.guard(["3-1"], evset)

        def leak():
            secret = m._secret(3)
            t0 = time.time()
            got = self.leak(m, p3.leak_byte, range(len(secret)), BUDGET_P3)
            n = sum(g == s for g, s in zip(got, secret))
            self.record("3-2", n >= 0.5 * len(secret),
                        f"short-window Spectre leaked {n}/{len(secret)} bytes "
                        f"({100 * n / len(secret):.0f}%, need >= 50%)  {time.time() - t0:.1f}s")
            self.note(f"leaked : {self.show(got)}")
            if self.verbose or n < len(secret):
                self.note(f"secret : {secret.decode()}")
        self.guard(["3-2"], leak)

    def part4(self):
        items = ["4-fence", "4-slh", "4-invisispec", "4-stt"]
        try:
            p4 = self.module("part4_defenses")
            attack = self.module("part2_spectre_v1", self.attack_impl).leak_byte
        except Exception as e:
            self.fail(items, e)
            return
        try:
            arch = self.module("part1_flush_reload", self.attack_impl).leak_byte
        except Exception:
            arch = None
        seed = self.seed + 4
        k = len(P4_BYTES)
        names = [b for b, _ in BENCHMARKS]

        def evaluate(kw, upass):
            m = Machine(seed=seed, **kw)
            secret = m._secret(2)
            got = self.leak(m, attack, P4_BYTES, BUDGET_P4)
            leaked = sum(got[j] == secret[i] for j, i in enumerate(P4_BYTES))
            archs = "n/a"
            if arch is not None:       # informational: does Part 1 still leak?
                try:
                    ma = Machine(seed=seed + 100, **kw)
                    s1 = ma._secret(1)
                    g1 = self.leak(ma, arch, range(5, 9), BUDGET_P1)
                    archs = f"{sum(g == s1[i] for g, i in zip(g1, range(5, 9)))}/4"
                except Exception:
                    archs = "err"
            res = run_suite(lambda: Machine(seed=seed, **kw), upass, seed=seed)
            return leaked, archs, res

        def row(name, leaked, archs, res, base):
            ratios = {b: res[b][0] / base[b][0] for b in names}
            geo = math.exp(sum(math.log(r) for r in ratios.values()) / len(ratios)) - 1
            correct = all(ok for _, ok in res.values())
            print(f"{'':16s}{name:11s} {leaked:>4d}/{k:<2d} {archs:>8s} {'ok' if correct else 'WRONG':>6s} "
                  f"{100 * geo:+7.1f}%  " + " ".join(f"{100 * (ratios[b] - 1):+7.0f}%" for b in names))
            return geo, correct, [b for b, (_, ok) in res.items() if not ok]

        print(f"{'':16s}Part-2 attack from {self.attack_impl}/ on secret bytes "
              f"{P4_BYTES.start}..{P4_BYTES.stop - 1}; software passes also recompile the benchmarks")
        print(f"{'':16s}{'defense':11s} {'leaked':>7s} {'arch F+R':>8s} {'bench':>6s} {'geomean':>8s}  "
              + " ".join(f"{n:>8s}" for n in names))
        try:
            leaked, archs, base = evaluate({}, None)
        except Exception as e:
            self.fail(items, e)
            return
        row("baseline", leaked, archs, base, base)
        if leaked < 0.8 * k:
            for it in items:
                self.record(it, False, "Part 2 attack does not work on the undefended machine")
            self.note(f"your Part 2 attack leaked only {leaked}/{k} bytes without any defense, so "
                      "blocking it proves nothing.")
            self.note("Fix Part 2 first, or grade your defenses with the reference attack: "
                      "make check P4_ATTACK=solutions")
            return
        configs = [
            ("fence", lambda: {"kernel_pass": p4.fence_pass}, p4.fence_pass),
            ("slh", lambda: {"kernel_pass": p4.slh_pass}, p4.slh_pass),
            ("invisispec", lambda: {"policy": p4.InvisiSpecPolicy()}, None),
            ("stt", lambda: {"policy": p4.STTPolicy()}, None),
        ]
        results = []
        for name, kwf, upass in configs:
            try:
                leaked, archs, res = evaluate(kwf(), upass)
                geo, correct, bad = row(name, leaked, archs, res, base)
                results.append((name, leaked, geo, correct, bad))
            except Exception as e:
                print(f"{'':16s}{name:11s} error")
                results.append((name, e))
        for r in results:
            item = f"4-{r[0]}"
            if len(r) == 2:
                self.fail([item], r[1])
                continue
            name, leaked, geo, correct, bad = r
            limit = OVERHEAD_LIMIT.get(name)
            fast = limit is None or geo <= limit
            self.record(item, leaked <= 1 and correct and fast,
                        f"{name}: attack leaked {leaked}/{k} (must be <= 1), benchmarks "
                        f"{'correct' if correct else 'WRONG: ' + ','.join(bad)}, overhead {100 * geo:+.1f}%"
                        + ("" if fast else f" (too slow: limit +{100 * limit:.0f}%; only speculative "
                           "*transmitters* / speculative cache *changes* should be affected)"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--impl", default="src", help="directory with your part*.py files (src or solutions)")
    ap.add_argument("--attack", default=os.environ.get("P4_ATTACK") or None,
                    help="directory whose part2 attack Part 4 uses (default: --impl)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--parts", default="1,2,3,4")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    seed = args.seed
    if seed is None:
        env = os.environ.get("SEED", "")
        seed = int(env) if env.strip() else int(time.time()) % 100000
    parts = [p.strip() for p in args.parts.split(",") if p.strip()]
    print(f"lab3-spectre-sim grader: impl={args.impl} seed={seed} parts={','.join(parts)}"
          f"   (repeat with SEED={seed})")
    t0 = time.time()
    g = Grader(args.impl, seed, args.verbose, args.attack)
    if not g.selftest():
        print("simulator selftest failed: your specsim/ copy is broken")
        return 2
    graded = []
    for p in parts:
        graded += [k for k in POINTS if k.split("-")[0] == p]
        getattr(g, f"part{p}")()
    total = sum(POINTS[k] for k in graded)
    got = sum(g.results.get(k, (False, 0))[1] for k in graded)
    allpass = all(g.results.get(k, (False, 0))[0] for k in graded)
    print(f"Score: {got:g}/{total:g}   ({'ALL PASS' if allpass else 'some parts FAIL'}, {time.time() - t0:.1f}s)")
    return 0 if allpass else 1


if __name__ == "__main__":
    sys.exit(main())
