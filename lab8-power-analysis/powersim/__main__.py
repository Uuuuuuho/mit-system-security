"""powersim command line.

    python3 -m powersim selftest
    python3 -m powersim show aes      [--seed S] [--noise X] [--model hw|hd] [--full] [--png out/aes.png]
    python3 -m powersim show modexp   [--seed S] [--noise X]
    python3 -m powersim show masked | delays | shuffle
    python3 -m powersim capture aes --n 1000 --out out/aes.npz [--noise X] [--model hd]
    python3 -m powersim dvfs [--seed S]
"""

import argparse
import os
import sys

import numpy as np

from . import dvfs, selftest
from .devices import AESDevice, MaskedSboxDevice, ModExpDevice
from .stats import save_plot, sparkline

DEVICES = ("aes", "modexp", "masked", "delays", "shuffle")


def make_device(name, a):
    if name == "aes":
        return AESDevice(seed=a.seed, model=a.model, noise=a.noise, window=None if a.full else (0, 200))
    if name == "masked":
        return MaskedSboxDevice(seed=a.seed, model=a.model, noise=a.noise)
    if name == "delays":
        return AESDevice(seed=a.seed, model=a.model, noise=a.noise, jitter=60, round_delay=40,
                         window=None if a.full else (0, 300))
    if name == "shuffle":
        return AESDevice(seed=a.seed, model=a.model, noise=a.noise, shuffle=True,
                         window=None if a.full else (0, 200))
    if name == "modexp":
        return ModExpDevice(seed=a.seed, nbits=a.bits, model=a.model, noise=a.noise)
    raise SystemExit(f"unknown device {name}; choose from {DEVICES}")


def cmd_show(a):
    dev = make_device(a.device, a)
    ts = dev.capture(a.n)
    tr = ts.traces
    print(f"{a.device}: {tr.shape[0]} trace(s) x {tr.shape[1]} samples, meta={ts.meta}")
    width = 100
    for i in range(min(a.n, 4)):
        print(f"trace {i}:")
        for s in range(0, tr.shape[1], width * a.per_char):
            seg = tr[i, s:s + width * a.per_char]
            print(f"  {s:6d} |{sparkline(seg, width=max(1, len(seg) // a.per_char), lo=tr.min(), hi=tr.max())}|")
    if ts.tags and a.tags:
        for s in range(0, min(len(ts.tags), a.tags)):
            print(f"  {s:5d}  {ts.tags[s]}")
    if a.png:
        os.makedirs(os.path.dirname(a.png) or ".", exist_ok=True)
        ok = save_plot(a.png, [tr[i] for i in range(min(a.n, 4))], title=f"powersim {a.device}",
                       ylabel="power (a.u.)")
        print(f"wrote {a.png}" if ok else "matplotlib not available: no PNG written")


def cmd_capture(a):
    dev = make_device(a.device, a)
    ts = dev.capture(a.n)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    if a.device == "modexp":
        np.savez_compressed(a.out, traces=ts.traces)
    else:
        ts.save(a.out)
    print(f"wrote {a.out}: traces {ts.traces.shape}")


def main(argv=None):
    p = argparse.ArgumentParser(prog="python3 -m powersim", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest")
    for name in ("show", "capture"):
        q = sub.add_parser(name)
        q.add_argument("device", choices=DEVICES)
        q.add_argument("--seed", type=int, default=None)
        q.add_argument("--noise", type=float, default=1.0)
        q.add_argument("--model", choices=("hw", "hd"), default="hw")
        q.add_argument("--full", action="store_true", help="capture the whole encryption (AES devices)")
        q.add_argument("--bits", type=int, default=128, help="exponent bits (modexp)")
        q.add_argument("--n", type=int, default=1)
        if name == "show":
            q.add_argument("--png", default=None)
            q.add_argument("--per-char", type=int, default=1, dest="per_char",
                           help="samples per character in the text view")
            q.add_argument("--tags", type=int, default=0, help="print the first N sample tags")
        else:
            q.add_argument("--out", required=True)
    q = sub.add_parser("dvfs")
    q.add_argument("--seed", type=int, default=None)
    a = p.parse_args(argv)
    if a.cmd == "selftest":
        return 0 if selftest.run() else 1
    if a.cmd == "show":
        cmd_show(a)
    elif a.cmd == "capture":
        cmd_capture(a)
    elif a.cmd == "dvfs":
        dvfs.demo(a.seed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
