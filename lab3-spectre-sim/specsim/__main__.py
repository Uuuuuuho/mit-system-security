"""Command line tools:

    python3 -m specsim selftest                    simulator sanity tests
    python3 -m specsim run prog.s [options]        run a user program on a fresh machine
    python3 -m specsim kernel                      print the kernel source and its symbols
    python3 -m specsim info                        memory map, cache geometry, latencies

`run` options:
    --seed N            machine seed (secrets, noise)
    --trace             print a per-instruction timing trace ('~' = transient)
    --set REG=VALUE     initial register value, e.g. --set a0=5 (repeatable)
    --entry LABEL       start label (default _start)
    --dump LABEL[:N]    after the run print N (default 8) u64 values at LABEL
"""

import argparse
import sys

from . import layout
from .machine import Machine, kernel_source
from .selftest import main as selftest_main


def _run(args):
    m = Machine(seed=args.seed)
    with open(args.file) as f:
        src = f.read()
    prog = m.load(src, name=args.file)
    regs = {}
    for item in args.set or []:
        k, _, v = item.partition("=")
        regs[k] = int(v, 0)
    res = m.run(prog, entry=args.entry, trace=print if args.trace else None, **regs)
    print(f"halted after {res.instrs} instructions, {res.cycles} cycles")
    print("  " + "  ".join(f"{r}={res[r]:#x}" for r in ("a0", "a1", "a2", "s2", "s3", "s4")))
    st = m._cpu.stats
    print(f"  mispredicts={st['mispredicts']} transient_instrs={st['transient']} "
          f"transient_loads={st['transient_loads']} syscalls={st['syscalls']}")
    for item in args.dump or []:
        label, _, n = item.partition(":")
        vals = m.read_u64s(prog[label], int(n or 8))
        print(f"  {label}: " + " ".join(str(v) for v in vals))
    return 0


def _kernel(_args):
    print(kernel_source())
    m = Machine(seed=0)
    print("# public kernel symbols:")
    for k, v in sorted(m.ksyms.items(), key=lambda kv: kv[1]):
        print(f"#   {k:14s} {v:#010x}")
    return 0


def _info(_args):
    m = Machine(seed=0)
    print(layout.__doc__)
    print("Cache geometry / latencies:", m.geometry)
    c = m.cfg
    print(f"Core: width={c.width} rob={c.rob} mispredict_penalty={c.mispredict_penalty} "
          f"syscall_latency={c.syscall_latency} pht_bits={c.pht_bits} ghist_bits={c.ghist_bits}")
    print("Assembler constants:", ", ".join(f"{k}={v:#x}" for k, v in layout.PUBLIC_CONSTANTS.items()))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m specsim", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest")
    r = sub.add_parser("run")
    r.add_argument("file")
    r.add_argument("--seed", type=int, default=0)
    r.add_argument("--trace", action="store_true")
    r.add_argument("--set", action="append")
    r.add_argument("--entry", default="_start")
    r.add_argument("--dump", action="append")
    sub.add_parser("kernel")
    sub.add_parser("info")
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest_main()
    return {"run": _run, "kernel": _kernel, "info": _info}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
