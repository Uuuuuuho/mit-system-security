# lab6-cpu-fuzz — Finding defects in a buggy RV32I CPU

Related lecture: **L12 Fuzzing and Bug Finding** (and L13, whose lab7 finds the same kinds of defects with formal
verification). Platform: [`../platform/psp-lite`](../platform/psp-lite/README.md), a RV32I instruction-set simulator
with machine/user privilege. Runs anywhere (x86_64 or aarch64), no timing involved.

## Why this lab
Hardware is usually the root of trust, yet processors ship with errata. The 1994 Pentium FDIV flaw (a few missing
entries in the SRT divider's lookup table) produced wrong quotients for rare operand pairs; Biham et al.'s *Bug
Attacks* showed that one such arithmetic defect can be enough to leak a secret key from correct cryptographic
software. CPUs also contain **undocumented behaviour**: debug leftovers, unlisted opcodes, or deliberately hidden
features. Because exhaustive testing of a modern core is impossible, the industry relies on testing strategies that
scale: exhaustive checks over *small* operand spaces, **differential testing** against a reference model, and
**coverage-guided fuzzing** of instruction streams (Trippel et al., *Fuzzing Hardware Like Software*; Google's
*SiliFuzz*).

Your CPU is a psp-lite build with **three hidden defects** derived from a random `SEED`: an arithmetic defect, an
undocumented instruction encoding, and a decode defect. The grader builds a CPU with a *fresh* seed, so your code
must discover the defects rather than hard-code them. `--golden` runs the same binary with every defect disabled.

## Setup
```
make                 # builds YOUR CPU (build/cpu/psp-lite, seed remembered in build/.seed) + your programs
make run1|run2|run3|run4
make check           # grade your code on a fresh-seed CPU     make check-solution   # grade the reference
make SEED=1234       # rebuild with a chosen seed (rm build/.seed to draw a new random one)
```
Programs are bare-metal C/assembly built with clang (`--target=riscv32 -march=rv32i`) + ld.lld; every program also
gets a disassembly next to the ELF (`build/student/partN/*.dis`). Useful simulator flags: `--golden`, `--trace`,
`--max-instr N`, `--stats`, `--dump-regs` (see the platform README).

Do **not** read `platform/psp-lite/src/defects.c` — it implements the defects (the parameters come from the seed, but
reading it spoils the lab).

## Part 1 — Warm-up: a wrong shortest path (15 %, ~45 min)
`part1/sp.c` runs Floyd–Warshall over a fixed road network. `make run1` prints the distances from node 0 on your CPU
and on the golden CPU — they differ.

- **Exercise 1-1.** Find which additions go wrong. Compute each candidate path length a second way that does not use
  the hardware adder (e.g. a ripple-carry adder from AND/XOR/shift), compare with `psp_hw_add()`, print the first few
  mismatches as `bad_add a=0x… b=0x… got=0x… want=0x…`, and continue with the correct value so your final output
  equals the golden run. Check the `.dis` file: the compiler must not have turned your reference back into `add`.
- **Discussion 1-2.** Which operand pairs fail? Is the failure universal or input-specific? What would a user of this
  CPU have observed in practice?

## Part 2 — Exhaustive differential testing of ADD (25 %, ~1.5 h)
- **Discussion 2-1.** How can a program test the adder when every loop counter, address computation and comparison it
  executes may itself use the adder?
- **Exercise 2-2.** Complete `part2/addscan.c`: test every pair a, b ∈ [0x100, 0x500) (a outer, b inner, ascending)
  against your reference adder and print the required summary (`count=`, `first …`, `pattern nibble_a= nibble_b=
  bit=`, `iterations=`). Advance counters without `add` and cross-check the iteration count.
- **Discussion 2-3.** Characterize the defect from your data (which operand bits matter, which result bit flips).
  Why would random testing of 32-bit operands almost never find it? What does that say about test-space reduction?

## Part 3 — Hunting undocumented instructions (30 %, ~2 h)
RISC-V reserves the `custom-0` major opcode (0x0B) for vendor extensions. psp-lite documents exactly one: `psp.id`
(funct3 = funct7 = 0). Every other encoding must raise an illegal-instruction exception.

- **Exercise 3-1.** Write the machine-mode trap handler in `part3/trap.S`: record `mcause` in `last_cause`,
  increment `trap_count`, resume after the faulting instruction (`mepc + 4`) and `mret`, preserving every register
  you touch. `part3/scan.c` already installs it and drops to user mode.
- **Exercise 3-2.** In `part3/scan.c`, execute every R-type custom-0 encoding (funct7 × funct3, registers x0) from
  user mode and print `undocumented=0x…` for each encoding that does **not** trap (or `undocumented=none`).
- **Discussion 3-3.** You found an instruction the specification says should not exist. What could it be, how would
  you characterise its behaviour safely (which architectural state would you compare before/after it?), and what
  would you ask the vendor? Why is "it does not trap" already a security finding for a privilege-separated design?
  (lab7 shows how a formal miter turns this question into a proof obligation.)

## Part 4 — Differential instruction-stream fuzzing (30 %, ~2 h) — beyond the course lab
`part4/rv32.py` (provided) encodes/decodes RV32I, builds flat test images with chosen initial registers and data,
runs psp-lite with `--trace`, and parses trace lines.

- **Exercise 4-1.** `gen_program()`: generate random instruction sequences that cover many mnemonics (forward-only
  control flow, memory accesses relative to `rv32.BASE_REG`), with initial values biased towards interesting numbers
  (0, ±1, sign boundaries, all-ones…).
- **Exercise 4-2.** `first_divergence()`: compare the golden and DUT traces.
- **Exercise 4-3.** Classify each divergence by the golden instruction's mnemonic, rebuild the architectural state
  just before it from the golden trace, and keep only findings whose **one-instruction reproducer** still diverges.
  Print `FOUND <mnemonic> insn=0x…` once per mnemonic. `make run4` runs your fuzzer; the grader requires that it finds
  the decode defect within ~100 s.
- **Discussion 4-4.** Report your coverage (mnemonics executed) and how it changed as you improved the generator.
  Which bugs would this fuzzer *never* find (think: timing, microarchitectural state, multi-instruction
  interactions, privilege transitions)? What would "coverage" mean for an RTL design (mux toggles, FSM states)?

## Going further
- Add operand-value coverage (e.g. which nibble pairs reached the adder) and steer the generator towards uncovered
  bins. Compare time-to-detection for the adder defect with and without feedback.
- Implement the software-side countermeasure idea from SPECS: duplicate security-critical arithmetic with an
  independent implementation and compare.
- Re-run Part 4 against a psp-lite built with `DEFECTS=0`: a correct fuzzer reports nothing.

## References
- T. R. Nicely, Pentium FDIV flaw (1994); A. Edelman, *The Mathematics of the Pentium Division Bug*, SIAM Review 1997.
- E. Biham, Y. Carmeli, A. Shamir, *Bug Attacks*, CRYPTO 2008.
- R. Kande et al., *TheHuzz*; T. Trippel et al., *Fuzzing Hardware Like Software*, USENIX Security 2022.
- K. Serebryany et al., *SiliFuzz: Fuzzing CPUs by proxy*, 2021. C. Domas, *Breaking the x86 ISA*, Black Hat 2017.
- M. Hicks et al., *SPECS: A Lightweight Runtime Mechanism for Protecting Software from Security-Critical Processor
  Bugs*, ASPLOS 2015. G. Dessouky et al., *HardFails*, USENIX Security 2019.

Spoilers: `solutions/` contains reference implementations for every part.
