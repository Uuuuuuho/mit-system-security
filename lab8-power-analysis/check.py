#!/usr/bin/env python3
"""Grader for lab8-power-analysis.

    python3 check.py                       grade src/ (your code)
    python3 check.py --impl solutions      grade the reference solution
    python3 check.py --parts 3,4 --seed 7 -v

Keys, exponents, masks and noise are drawn fresh for every run (SEED env var or --seed to
repeat a run).  Exit status is 0 only if every graded item passes (the optional item 4-2 is a
bonus and only required with --require-bonus).
"""

import argparse
import importlib
import os
import sys
import time
import traceback
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from powersim import (AESDevice, MaskedSboxDevice, ModExpDevice, Recorder, SBOX, HW8,  # noqa: E402
                      aes_encrypt, expand_key)
from powersim import selftest as sim_selftest                                          # noqa: E402
from powersim.stats import welch_t                                                     # noqa: E402

POINTS = {"1-1": 7, "1-2": 8, "1-3": 5,
          "2-1": 8, "2-2": 12,
          "3-1": 8, "3-2": 8, "3-3": 6, "3-4": 8,
          "4-1": 14, "4-2": 0, "4-3": 10, "4-4": 6}
BONUS = {"4-2"}

# Device parameters and trace budgets (the reference solution needs a fraction of each budget)
P1_NOISE, P1M_NOISE, P1M_JITTER, P1M_TRACES = 1.0, 8.0, 300, 25
P2_NOISE, P2_TRACES = 2.0, 3000
P3_NOISE, P3_TRACES = 3.0, 400
P3HD_NOISE, P3HD_TRACES = 2.0, 600
P3C_NOISE, P3C_TRACES = 4.0, 1500
P4_TVLA_N, P4_TVLA_NOISE, P4_CPA_N = 1500, 0.5, 2000
P4_SO_NOISE, P4_SO_TRACES = 1.0, 4000
P4_DELAY = dict(noise=1.5, jitter=60, round_delay=40, window=(0, 300))
P4_DELAY_TRACES = 1000
P4_SHUF_NOISE, P4_SHUF_TRACES = 3.0, 3000


# ---------------------------------------------------------------------------- grader's own tools
def ref_pearson(T, H):
    T = np.asarray(T, dtype=np.float64)
    H = np.asarray(H, dtype=np.float64)
    T = T - T.mean(axis=0)
    H = H - H.mean(axis=0)
    num = H.T @ T
    den = np.sqrt((H * H).sum(axis=0))[:, None] * np.sqrt((T * T).sum(axis=0))[None, :]
    return np.where(den > 0, num / np.where(den > 0, den, 1), 0.0)


def ref_hyp(pts, b, model="hw"):
    x = pts[:, b].astype(np.intp)[:, None] ^ np.arange(256)[None, :]
    return HW8[SBOX[x]] if model == "hw" else HW8[x ^ SBOX[x]]


def ref_scores(traces, pts, model="hw"):
    return np.stack([np.abs(ref_pearson(traces, ref_hyp(pts, b, model))).max(axis=1) for b in range(16)])


def ref_cpa_key(traces, pts, model="hw"):
    return ref_scores(traces, pts, model).argmax(axis=1).astype(np.uint8)


def ref_dom(traces, pts, byte, bit):
    T = np.asarray(traces, dtype=np.float64)
    x = pts[:, byte].astype(np.intp)[:, None] ^ np.arange(256)[None, :]
    sel = ((SBOX[x] >> bit) & 1).astype(np.float64)
    n1 = sel.sum(axis=0)
    s1 = sel.T @ T
    s0 = T.sum(axis=0)[None, :] - s1
    return s1 / n1[:, None] - s0 / (T.shape[0] - n1)[:, None]


def ref_ranks(traces, pts, key, ns):
    out = []
    for n in ns:
        sc = ref_scores(traces[:n], pts[:n])
        out.append([int((sc[b] > sc[b, key[b]]).sum()) for b in range(16)])
    return np.array(out)


def as_key(k):
    if k is None:
        raise ValueError("returned None instead of a key")
    if isinstance(k, (bytes, bytearray)):
        k = list(k)
    k = np.asarray(k).astype(np.int64).ravel()
    if k.shape != (16,):
        raise ValueError(f"key must have 16 bytes, got shape {k.shape}")
    return (k & 0xFF).astype(np.uint8)


def hexkey(k):
    return bytes(np.asarray(k, dtype=np.uint8).tolist()).hex()


# ---------------------------------------------------------------------------- grader
class Grader:
    def __init__(self, impl, seed, verbose):
        self.impl = impl
        self.seed = seed
        self.verbose = verbose
        self.results = {}
        d = os.path.join(HERE, impl)
        if not os.path.isdir(d):
            raise SystemExit(f"no such directory: {d}")
        sys.path.insert(0, d)

    def rng(self, item):
        return np.random.default_rng([self.seed, zlib.crc32(item.encode())])

    def module(self, name):
        return importlib.import_module(name)

    def record(self, item, passed, msg):
        if item in self.results:
            return
        pts = POINTS[item] if passed else 0
        self.results[item] = (passed, pts)
        if item in BONUS:
            status = "BONUS PASS" if passed else ("SKIP" if passed is None else "BONUS FAIL")
            print(f"[{item:>4s}] {status:10s}          {msg}")
        else:
            print(f"[{item:>4s}] {'PASS' if passed else 'FAIL':10s} ({pts:2d}/{POINTS[item]:2d})  {msg}")

    def note(self, msg):
        print(f"{'':25s}{msg}")

    def guard(self, items, fn):
        t0 = time.time()
        try:
            fn()
        except Exception as e:                      # student code may raise anything
            kind = type(e).__name__
            first = str(e).splitlines()[0] if str(e) else ""
            for it in items:
                self.record(it, False, f"{kind}: {first}")
            traceback.print_exception(type(e), e, e.__traceback__, limit=None if self.verbose else -2)
        if self.verbose:
            self.note(f"({time.time() - t0:.1f} s)")

    # ------------------------------------------------------------------ Part 1
    def part1(self):
        p1 = self.module("part1_spa")

        def ex11():
            dev = ModExpDevice(rng=self.rng("1-1"), noise=P1_NOISE)
            ts = dev.capture(1)
            e = p1.recover_exponent(ts.traces[0], dev.nbits, dev.nwords)
            e = int(e) if e is not None else -1
            wrong = bin((e ^ dev._e) & ((1 << dev.nbits) - 1)).count("1") if e >= 0 else dev.nbits
            ok = e == dev._e
            self.record("1-1", ok, f"single clean trace: {dev.nbits - wrong}/{dev.nbits} exponent bits correct"
                        + ("" if ok else " (need all)"))
        self.guard(["1-1"], ex11)

        def ex12():
            dev = ModExpDevice(rng=self.rng("1-2"), noise=P1M_NOISE, jitter=P1M_JITTER)
            ts = dev.capture(P1M_TRACES)
            e = p1.recover_exponent_multi(ts.traces, dev.nbits, dev.nwords)
            e = int(e) if e is not None else -1
            wrong = bin((e ^ dev._e) & ((1 << dev.nbits) - 1)).count("1") if e >= 0 else dev.nbits
            ok = e == dev._e
            self.record("1-2", ok, f"{P1M_TRACES} traces, noise {P1M_NOISE}, jitter 0..{P1M_JITTER}: "
                        f"{dev.nbits - wrong}/{dev.nbits} bits correct" + ("" if ok else " (need all)"))
        self.guard(["1-2"], ex12)

        def ex13():
            rng = self.rng("1-3")
            nbits, mod_bits = 64, 128
            nwords = mod_bits // 32
            mod = int.from_bytes(rng.bytes(mod_bits // 8), "big") | (1 << (mod_bits - 1)) | 1
            seqs = []
            for _ in range(3):
                e = int.from_bytes(rng.bytes(nbits // 8), "big") | (1 << (nbits - 1))
                bases = [int.from_bytes(rng.bytes(mod_bits // 8), "big") % mod for _ in range(6)]
                cpu = Recorder(len(bases), "hw", rng=rng)
                out = p1.ladder(cpu, bases, e, mod, nbits, nwords)
                if out is None or [int(o) for o in out] != [pow(b, e, mod) for b in bases]:
                    self.record("1-3", False, "ladder() returned wrong results (must equal pow(b, e, mod))")
                    return
                seqs.append(list(cpu.kinds))
            nbig = sum(k in ("sqr", "mul") for k in seqs[0]) / nwords
            regular = seqs[0] == seqs[1] == seqs[2]
            ok = regular and nbig >= 2 * nbits
            why = ("same instruction sequence for 3 random exponents" if regular
                   else "instruction sequence depends on the exponent")
            self.record("1-3", ok, f"results correct; {why}; {nbig:.0f} big-number ops for {nbits} bits"
                        + ("" if nbig >= 2 * nbits else " (expected >= 2 per bit)"))
        self.guard(["1-3"], ex13)

    # ------------------------------------------------------------------ Part 2
    def part2(self):
        p2 = self.module("part2_dpa")

        def ex21():
            rng = self.rng("2-1")
            dev = AESDevice(rng=rng, noise=P2_NOISE)
            ts = dev.capture(500)
            byte, bit = int(rng.integers(16)), int(rng.integers(8))
            got = np.asarray(p2.dom_traces(ts.traces, ts.pts, byte, bit), dtype=np.float64)
            want = ref_dom(ts.traces, ts.pts, byte, bit)
            if got.shape != want.shape:
                self.record("2-1", False, f"dom_traces() shape {got.shape}, expected {want.shape}")
                return
            err = float(np.nanmax(np.abs(got - want)))
            self.record("2-1", err < 1e-3, f"dom_traces(byte={byte}, bit={bit}) max |error| = {err:.2e}")
        self.guard(["2-1"], ex21)

        def ex22():
            dev = AESDevice(rng=self.rng("2-2"), noise=P2_NOISE)
            ts = dev.capture(P2_TRACES)
            t0 = time.time()
            k = as_key(p2.dpa_recover_key(ts.traces, ts.pts))
            good = int((k == dev._key).sum())
            self.record("2-2", good == 16, f"DPA with {P2_TRACES} traces (noise {P2_NOISE}): {good}/16 key bytes "
                        f"({time.time() - t0:.1f} s)")
            if good < 16 or self.verbose:
                self.note(f"got     {hexkey(k)}")
                self.note(f"correct {hexkey(dev._key)}")
        self.guard(["2-2"], ex22)

    # ------------------------------------------------------------------ Part 3
    def part3(self):
        p3 = self.module("part3_cpa")

        def ex31():
            rng = self.rng("3-1")
            T = rng.normal(size=(300, 50)) + rng.normal(size=(300, 1)) * 0.5
            H = rng.integers(0, 9, size=(300, 37)).astype(np.float64)
            T[:, 7] += 0.8 * H[:, 3]
            got = np.asarray(p3.pearson(T.astype(np.float32), H), dtype=np.float64)
            want = ref_pearson(T.astype(np.float32), H)
            if got.shape != want.shape:
                self.record("3-1", False, f"pearson() shape {got.shape}, expected {want.shape} (G, S)")
                return
            err = float(np.nanmax(np.abs(got - want)))
            self.record("3-1", err < 1e-4, f"pearson() on random data: max |error| = {err:.2e}")
        self.guard(["3-1"], ex31)

        def ex32():
            dev = AESDevice(rng=self.rng("3-2"), noise=P3_NOISE)
            ts = dev.capture(P3_TRACES)
            k = as_key(p3.cpa_recover_key(ts.traces, ts.pts))
            good = int((k == dev._key).sum())
            self.record("3-2", good == 16, f"CPA (HW model) with {P3_TRACES} traces, noise {P3_NOISE}: {good}/16 bytes")
            if good < 16:
                self.note(f"got     {hexkey(k)}\n{'':25s}correct {hexkey(dev._key)}")
        self.guard(["3-2"], ex32)

        def ex33():
            dev = AESDevice(rng=self.rng("3-3"), model="hd", noise=P3HD_NOISE)
            ts = dev.capture(P3HD_TRACES)
            k = as_key(p3.cpa_recover_key(ts.traces, ts.pts, model="hd"))
            good = int((k == dev._key).sum())
            self.record("3-3", good == 16, f"CPA on the HD-model device, {P3HD_TRACES} traces: {good}/16 bytes")
            try:
                k2 = as_key(p3.cpa_recover_key(ts.traces, ts.pts, model="hw"))
                self.note(f"(for comparison, your HW-model attack on the same traces: "
                          f"{int((k2 == dev._key).sum())}/16)")
            except Exception:
                pass
        self.guard(["3-3"], ex33)

        def ex34():
            dev = AESDevice(rng=self.rng("3-4"), noise=P3C_NOISE)
            ts = dev.capture(P3C_TRACES)
            ns = sorted(set(int(round(x)) for x in np.geomspace(5, P3C_TRACES, 24)))
            t0 = time.time()
            got = np.asarray(p3.rank_curve(ts.traces, ts.pts, dev._key.copy(), ns))
            dt = time.time() - t0
            if got.shape != (len(ns), 16):
                self.record("3-4", False, f"rank_curve() shape {got.shape}, expected {(len(ns), 16)}")
                return
            got = got.astype(int)
            need = p3.traces_needed(got, ns)
            ok_rows = (got == 0).all(axis=1)
            consistent = None
            if ok_rows[-1]:
                i = len(ns) - 1
                while i > 0 and ok_rows[i - 1]:
                    i -= 1
                consistent = ns[i]
            want = ref_ranks(ts.traces, ts.pts, dev._key, ns)
            ref_need = ns[[i for i in range(len(ns)) if (want[i:] == 0).all()][0]] \
                if (want[-1] == 0).all() else None
            dist = float(np.abs(np.log2(got + 1) - np.log2(want + 1)).mean())
            checks = [
                (((0 <= got) & (got <= 255)).all(), "ranks must be in 0..255"),
                (ok_rows[-1], f"all 16 bytes should have rank 0 with {ns[-1]} traces"),
                (got[0].mean() > 20, f"with {ns[0]} traces the ranks should look random (mean {got[0].mean():.1f})"),
                (need == consistent, f"traces_needed() = {need}, but your ranks say {consistent}"),
                (dist < 1.5, f"ranks differ from the grader's CPA by {dist:.2f} bits on average"),
            ]
            if ref_need is not None and need is not None:
                checks.append((need <= 3 * ref_need, f"traces_needed {need} vs grader {ref_need}"))
            bad = [m for ok, m in checks if not ok]
            self.record("3-4", not bad, f"rank curve over {len(ns)} points ({dt:.1f} s); traces needed: "
                        f"yours {need}, grader {ref_need}" + ("" if not bad else "; " + bad[0]))
            if self.verbose or (bad and got.any()):
                for n, r, w in zip(ns, got, want):
                    self.note(f"n={n:5d}  mean rank {r.mean():7.1f}  (grader {w.mean():7.1f})")
        self.guard(["3-4"], ex34)

    # ------------------------------------------------------------------ Part 4
    def part4(self):
        p4 = self.module("part4_countermeasures")

        def ex41():
            rng = self.rng("4-1")
            key = rng.integers(0, 256, 16, dtype=np.uint8)
            rk = expand_key(key)

            def run(pt, model="hw"):
                m0 = rng.integers(0, 256, pt.shape, dtype=np.uint8)
                cpu = Recorder(len(pt), model, rng=rng)
                c0, c1 = p4.masked_aes(cpu, pt ^ m0, m0, rk.copy())
                c = np.asarray(c0, dtype=np.uint8) ^ np.asarray(c1, dtype=np.uint8)
                if c.shape != pt.shape or not (c == aes_encrypt(pt, key)).all():
                    raise AssertionError("masked_aes(): c0 ^ c1 != AES_k(p0 ^ p1)")
                return cpu
            pt = rng.integers(0, 256, (64, 16), dtype=np.uint8)
            cpu = run(pt)
            nlook = sum(k == "lookup" for k in cpu.kinds)
            if nlook < 160 or len(cpu) < 1000:
                self.record("4-1", False, f"only {len(cpu)} instructions / {nlook} table lookups: route every "
                            f"operation on (masked) data through cpu.* (expected >= 1000 / >= 160)")
                return
            # fixed-vs-random TVLA, two independent repetitions
            fixed = rng.integers(0, 256, 16, dtype=np.uint8)
            ts = []
            for rep in range(2):
                g = []
                for kind in ("fixed", "random"):
                    p = np.tile(fixed, (P4_TVLA_N, 1)) if kind == "fixed" else \
                        rng.integers(0, 256, (P4_TVLA_N, 16), dtype=np.uint8)
                    g.append(run(p).render(noise=P4_TVLA_NOISE, rng=rng))
                if g[0].shape != g[1].shape:
                    raise AssertionError("trace length depends on the data")
                ts.append(welch_t(g[0], g[1]))
            leak = np.nonzero((np.abs(ts[0]) > 4.5) & (np.abs(ts[1]) > 4.5))[0]
            tags = run(pt[:4]).tags
            # first-order CPA with the grader's attack
            p = rng.integers(0, 256, (P4_CPA_N, 16), dtype=np.uint8)
            tr = run(p).render(noise=1.0, rng=rng)
            kc = ref_cpa_key(tr, p)
            hits = int((kc == key).sum())
            ok = len(leak) == 0 and hits <= 2
            self.record("4-1", ok, f"correct; {len(cpu)} instructions; TVLA (HW, {P4_TVLA_N}/group): "
                        f"{len(leak)} leaking samples, max|t| {np.abs(ts[0]).max():.1f}/{np.abs(ts[1]).max():.1f}; "
                        f"1st-order CPA ({P4_CPA_N} traces): {hits}/16 bytes")
            for s in leak[:10]:
                self.note(f"leaks: sample {s:5d} t = {ts[0][s]:+7.1f} {ts[1][s]:+7.1f}  {tags[s] if s < len(tags) else ''}")
            if len(leak) > 10:
                self.note(f"... and {len(leak) - 10} more")
        self.guard(["4-1"], ex41)

        def ex42():
            dev = MaskedSboxDevice(rng=self.rng("4-2"), noise=P4_SO_NOISE)
            ts = dev.capture(P4_SO_TRACES)
            k = p4.second_order_attack(ts.traces, ts.pts, list(ts.tags))
            if k is None:
                self.record("4-2", None, "second_order_attack() returned None (optional exercise skipped)")
                return
            k = as_key(k)
            good = int((k == dev._key).sum())
            first = int((ref_cpa_key(ts.traces, ts.pts) == dev._key).sum())
            self.record("4-2", good == 16, f"2nd-order CPA, {P4_SO_TRACES} traces: {good}/16 bytes "
                        f"(1st-order CPA on the same traces: {first}/16)")
        self.guard(["4-2"], ex42)

        def ex43():
            dev = AESDevice(rng=self.rng("4-3"), **P4_DELAY)
            ts = dev.capture(P4_DELAY_TRACES)
            plain = int((ref_cpa_key(ts.traces, ts.pts) == dev._key).sum())
            al = np.asarray(p4.align(ts.traces.copy()), dtype=np.float64)
            if al.ndim != 2 or al.shape[0] != len(ts) or al.shape[1] < 16:
                self.record("4-3", False, f"align() must return (n, L) with L >= 16, got {al.shape}")
                return
            ga = int((ref_cpa_key(al, ts.pts) == dev._key).sum())
            k = as_key(p4.attack_delays(ts.traces.copy(), ts.pts))
            gk = int((k == dev._key).sum())
            self.record("4-3", ga == 16 and gk == 16,
                        f"{P4_DELAY_TRACES} traces, jitter 0..{P4_DELAY['jitter']}, delays 0..{P4_DELAY['round_delay']}: "
                        f"grader CPA on raw {plain}/16, on your align() output {ga}/16; attack_delays() {gk}/16")
        self.guard(["4-3"], ex43)

        def ex44():
            dev = AESDevice(rng=self.rng("4-4"), noise=P4_SHUF_NOISE, shuffle=True)
            ts = dev.capture(P4_SHUF_TRACES)
            plain = int((ref_cpa_key(ts.traces, ts.pts) == dev._key).sum())
            k = as_key(p4.attack_shuffled(ts.traces, ts.pts, list(ts.tags)))
            good = int((k == dev._key).sum())
            self.record("4-4", good == 16, f"shuffled device, {P4_SHUF_TRACES} traces: plain CPA {plain}/16, "
                        f"attack_shuffled() {good}/16")
        self.guard(["4-4"], ex44)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--impl", default="src", help="directory with your part*.py files (src or solutions)")
    ap.add_argument("--parts", default="1,2,3,4")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--require-bonus", action="store_true", help="also require the optional item 4-2")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    seed = a.seed
    if seed is None and os.environ.get("SEED"):
        seed = int(os.environ["SEED"])
    if seed is None:
        seed = int.from_bytes(os.urandom(4), "little")
    parts = [int(p) for p in a.parts.split(",") if p.strip()]
    print(f"lab8-power-analysis grader: impl={a.impl} parts={parts} seed={seed}  (repeat with SEED={seed})")
    t0 = time.time()
    if not sim_selftest.run(verbose=False):
        print("powersim selftest failed: the simulator itself is broken")
        return 2
    g = Grader(a.impl, seed, a.verbose)
    for p in parts:
        print(f"--- Part {p} " + "-" * 60)
        try:
            getattr(g, f"part{p}")()
        except Exception as e:                       # e.g. a syntax error in the student's module
            items = [k for k in POINTS if k.startswith(f"{p}-")]
            for it in items:
                g.record(it, False, f"could not load your Part {p} module: {type(e).__name__}: {e}")
            traceback.print_exception(type(e), e, e.__traceback__, limit=-2)
    graded = [k for k in POINTS if int(k[0]) in parts and k not in BONUS]
    score = sum(g.results.get(k, (False, 0))[1] for k in graded)
    total = sum(POINTS[k] for k in graded)
    passed = all(g.results.get(k, (False, 0))[0] for k in graded)
    if a.require_bonus:
        for b in BONUS:
            if int(b[0]) in parts and not g.results.get(b, (False, 0))[0]:
                passed = False
    print("-" * 72)
    print(f"score: {score}/{total}   {'ALL PASS' if passed else 'NOT YET'}   ({time.time() - t0:.1f} s, seed {seed})")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
