#!/usr/bin/env python3
"""collect.py - run every victim site N times next to a collector and save the traces.

For each trace:
  1. start the collector with --wait-start (it allocates and warms its buffer, prints "ready"),
  2. start the site (it builds its data structures, prints "ready"),
  3. tell the collector to go: window 0 starts (within a few ms of) when the site starts,
  4. wait for both, parse the collector's trace file.

Traces are collected round-robin over the sites in a shuffled order, so slow drifts of the
machine (thermal state, other tenants, a browser tab you opened) do not line up with one label.

CPU layouts (--layout):
  shared  collector and site both pinned to CPU 0   (default) They time-share one core, like a
          browser tab and an attacker tab that land on the same core. Every other CPU stays
          free for this script and the rest of the system, so it works on a 2-CPU box.
  split   collector on CPU 0, site on CPU 1          Only shared caches / memory / power couple
          the two (needs >= 2 CPUs, else behaves like none). Part 3 compares the layouts.
  none    no pinning; the OS scheduler decides

Example:
  python3 tools/collect.py --collector build/collector --traces 20 --duration 3 --period 5 \
      --out traces/sweep.npz
"""
import argparse
import json
import os
import platform
import random
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fpdata  # noqa: E402

ALL_SITES = ["news", "social", "miner", "video", "docs", "webmail", "maps", "search"]


def cpus_for(layout):
    n = os.cpu_count() or 1
    if layout == "split" and n >= 2:
        return 0, 1
    if layout == "shared":
        return 0, 0
    return -1, -1


def usable_nice(want):
    """The nice value collector and site will get: `want` if this process may set it (negative
    values need root or CAP_SYS_NICE, which the lab's compose file grants), else 0."""
    if want >= 0:
        return want
    try:
        rc = subprocess.run([sys.executable, "-c", f"import os; os.setpriority(os.PRIO_PROCESS, 0, {want})"],
                            capture_output=True).returncode
    except OSError:
        rc = 1
    return want if rc == 0 else 0


def run_one(collector, site_bin, period, duration, buf, ccpu, vcpu, seed, tmpdir, nice=0):
    out = os.path.join(tmpdir, "trace.txt")
    # Both processes get the SAME nice value, so how they share a CPU with each other (the signal)
    # is unchanged; only unrelated background processes get a smaller share of that CPU.
    pre = (lambda: os.setpriority(os.PRIO_PROCESS, 0, nice)) if nice else None
    ccmd = [collector, "-P", str(period), "-T", str(duration), "-o", out, "--cpu", str(ccpu), "--wait-start"]
    if buf:
        ccmd += ["-b", str(buf)]
    # The site runs a little longer than the trace so it never exits inside the last window.
    vcmd = [site_bin, "--duration", str(duration + 0.3), "--seed", str(seed), "--cpu", str(vcpu)]
    col = subprocess.Popen(ccmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, preexec_fn=pre)
    vic = None
    try:
        if col.stdout.readline().strip() != "ready":
            raise RuntimeError("collector did not become ready (crashed?)")
        vic = subprocess.Popen(vcmd, stdout=subprocess.PIPE, text=True, preexec_fn=pre)
        if vic.stdout.readline().strip() != "ready":
            raise RuntimeError(f"site {site_bin} did not become ready")
        col.stdin.write("go\n")
        col.stdin.flush()
        if col.wait(timeout=duration + 30) != 0:
            raise RuntimeError(f"collector exited with {col.returncode}")
        if vic.poll() is None and not os.environ.get("FP_DEBUG_SITE_EXIT"):
            vic.terminate()  # the trace is complete; do not wait for the site's extra 0.3 s
        vic.wait(timeout=10)
    finally:
        for p in (col, vic):
            if p is not None and p.poll() is None:
                p.kill()
                p.wait()
    header, counts = fpdata.read_trace(out)
    return header, counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--collector", required=True, help="collector binary (build/collector or build/collector_counter)")
    ap.add_argument("--sites-dir", default=None, help="directory with the site binaries (default: <collector dir>/sites)")
    ap.add_argument("--sites", default=",".join(ALL_SITES), help="comma-separated site names")
    ap.add_argument("--traces", type=int, default=20, help="traces per site")
    ap.add_argument("--duration", type=float, default=3.0, help="trace length T in seconds")
    ap.add_argument("--period", type=float, default=5.0, help="window length P in ms")
    ap.add_argument("--buf", default=None, help="collector buffer size (e.g. 4M); default: collector decides")
    ap.add_argument("--layout", choices=["shared", "split", "none"], default="shared")
    ap.add_argument("--nice", type=int, default=int(os.environ.get("NICE", "-10")),
                    help="nice value for collector and site (default -10 = shielded from background jobs "
                         "when permitted, falls back to 0; env NICE)")
    ap.add_argument("--seed", type=int, default=None, help="seed for site jitter and order (default: time)")
    ap.add_argument("--out", required=True, help="output dataset (.npz)")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    sites = [s for s in a.sites.split(",") if s]
    sites_dir = a.sites_dir or os.path.join(os.path.dirname(os.path.abspath(a.collector)), "sites")
    for s in sites:
        if not os.access(os.path.join(sites_dir, s), os.X_OK):
            sys.exit(f"collect.py: missing site binary {os.path.join(sites_dir, s)} (run make)")
    seed = a.seed if a.seed is not None else int(time.time() * 1000) % (2**31)
    rng = random.Random(seed)
    ccpu, vcpu = cpus_for(a.layout)
    nice = usable_nice(a.nice)
    if nice != a.nice and not a.quiet:
        print(f"note: cannot set nice {a.nice} here (needs root or CAP_SYS_NICE); running at nice 0")

    plan = []
    for i in range(a.traces):
        order = list(range(len(sites)))
        rng.shuffle(order)
        plan += [(k, i) for k in order]

    X, y, header = [], [], {}
    t_start = time.time()
    with tempfile.TemporaryDirectory(prefix="fp-") as tmp:
        for n, (k, i) in enumerate(plan):
            header, counts = run_one(a.collector, os.path.join(sites_dir, sites[k]), a.period, a.duration,
                                     a.buf, ccpu, vcpu, rng.randrange(2**31), tmp, nice)
            X.append(counts)
            y.append(k)
            if not a.quiet:
                print(f"[{n + 1:4d}/{len(plan)}] {sites[k]:>8} #{i:<3d} mean={counts.mean():10.1f} "
                      f"min={counts.min():8.0f} max={counts.max():8.0f}", flush=True)
    W = min(len(r) for r in X)
    X = [r[:W] for r in X]
    meta = {"collector": header.get("collector", "?"), "P_ms": a.period, "T_s": a.duration, "layout": a.layout,
            "buf": header.get("buf"), "llc": header.get("llc"), "line": header.get("line"), "seed": seed,
            "traces_per_site": a.traces, "nice": nice, "machine": platform.machine(), "cpus": os.cpu_count(),
            "collected_at": time.strftime("%Y-%m-%d %H:%M:%S"), "wall_s": round(time.time() - t_start, 1)}
    fpdata.save_dataset(a.out, X, y, sites, meta)
    if not a.quiet:
        print(f"saved {len(X)} traces x {W} windows to {a.out}  ({json.dumps(meta)})")


if __name__ == "__main__":
    main()
