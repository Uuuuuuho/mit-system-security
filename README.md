# shd-labs — self-study labs for Secure Hardware Design

Original practice labs that follow the topics of MIT 6.5950/6.5951 *Secure Hardware Design* (Spring 2026,
https://shd.mit.edu/2026/). The course's own lab code is not public, so every lab here was written from scratch:
same skills and progression, different code, data and text. Each lab has a README (background, exercises,
discussion questions), a `report.md` template, starter code with `TODO`s, an autograder and reference solutions.

| Lab | Topic | Lecture | Runs on | `make check` |
|---|---|---|---|---|
| [lab0-c-warmup](lab0-c-warmup/README.md) | C crash course: fix 10 planted memory/UB bugs (ASan, UBSan, valgrind) | L01 | native | ~10 s |
| [lab1-fingerprint](lab1-fingerprint/README.md) | Sweep-counting "website" fingerprinting + root-cause analysis | L02 | native | ~2 min |
| [lab2-cache](lab2-cache/README.md) | Latency histograms, Flush+Reload, Prime+Probe, key leak, covert channel | L03 | **sim** (default) / native | ~15 s |
| [lab3-spectre-sim](lab3-spectre-sim/README.md) | Transient execution in a CPU model; fences, SLH, InvisiSpec-like, STT-like defenses | L04–L06 | sim | ~5 s |
| [lab4-rowhammer](lab4-rowhammer/README.md) | pagemap, bank-conflict timing, XOR bank functions, disturbance model, SECDED, PARA/Graphene | L08–L09 | pagemap native, rest sim | ~10 s |
| [lab6-cpu-fuzz](lab6-cpu-fuzz/README.md) | Finding seeded defects in an RV32I CPU: exhaustive, differential and instruction fuzzing | L12 | psp-lite ISS | ~10 s |
| [lab7-cpu-formal](lab7-cpu-formal/README.md) | Spec core + miter + bounded model checking (yosys/ABC) of a tiny core | L13 | yosys | ~1 min |
| [lab8-power-analysis](lab8-power-analysis/README.md) | SPA, DPA, CPA on synthetic traces; masking, shuffling, frequency channels | L07 | sim | ~10 s |
| [platform/psp-lite](platform/psp-lite/README.md) | RV32I simulator + bare-metal SDK (used by lab6); optional hardware shadow-stack exercise (L11) | L11–L12 | — | `make smoke` |

Lectures without a lab here: L10 Root of Trust, L14 TEE. The course's ASLR-bypass lab has no counterpart: its
defensive side (shadow stacks) is covered by the optional psp-lite exercise and lab0/L11.

## Getting started on a Mac (Docker)
```sh
./scripts/lab-shell.sh                     # first run builds the image (Ubuntu 24.04, native arm64)
make -C lab0-c-warmup check                # inside the container
./scripts/lab-make.sh -C lab3-spectre-sim check-solution     # or run one make target from macOS
```
The image is built for the **host architecture** on purpose (arm64 on Apple silicon). Do not force `linux/amd64`:
instruction emulation makes every timing measurement meaningless.

## Native Linux
Ubuntu 24.04 (or similar) with: `build-essential gdb valgrind python3-numpy python3-matplotlib python3-sklearn
python3-z3 clang lld llvm yosys iverilog` (the same list as the `Dockerfile`).

## Which labs measure real hardware?
`common/shd.h` gives portable timer/flush/fence helpers for x86_64 and aarch64; `make -C common run` shows whether
your machine exposes a usable cached-vs-flushed timing gap.

- **Apple silicon (Docker/VM, aarch64):** the generic timer ticks at 24 MHz, and single-access hit/miss timing is not
  measurable from a VM. Aggregate measurements still work. So lab0, lab1 and lab4 part 1 run natively, and lab2 uses
  its deterministic cache simulator (the default).
- **x86_64 Linux (bare metal is best):** additionally try `SHD_BACKEND=native` in lab2 (see its README for SMT/LLC
  notes and what was observed on a VM).
- lab3, lab4 (parts 2–6), lab6, lab7 and lab8 are simulators/formal tools and behave identically everywhere.

## Ground rules
- Attacks are confined to **cooperating toy programs and simulators** in this repository. Transient execution,
  DRAM disturbance and power analysis happen only inside models; nothing here targets the real kernel, real DRAM,
  browsers or other people's software.
- Every attack part is paired with a defense part or discussion.
- `solutions/` directories contain spoilers. `make check` grades your code; `make check-solution` grades the reference.

## Layout
```
common/            shd.h (portable timing helpers) + selftest
labN-*/            README.md  report.md  Makefile  starter code  solutions/  grader
platform/psp-lite  RV32I ISS + SDK
scripts/           lab-shell.sh, lab-make.sh (Docker wrappers)
Makefile           make check-solution / make check / make clean across all labs
```

## Companion slides
Lecture-by-lecture study notes (Figma Slides, one deck per lecture, files "SHD L01 … L14") live in the Figma team
folder https://www.figma.com/files/team/1653958574289302591/folder/685233239 ; each deck ends with a "Practice"
slide pointing to the matching lab.
