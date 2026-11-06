# Lab 7: Formal verification of a tiny CPU core

In lab 6 you hunted for planted defects in a buggy RV32I core by running lots of
programs on it. This lab goes after the same kinds of defect (a wrong ALU result
for a few magic operands, a hidden instruction that hands out privilege) from the
other side. You will write an executable **specification** of the ISA and wire
it to the **implementation** in a **miter**. A SAT solver then searches *every*
instruction sequence up to a bounded length for one where the two disagree. When
it finds one you get a concrete counterexample program. When it finds none, you
have a proof that holds for every program of that length.

The core under verification is `psp-mini`, a small multi-cycle RV32I-subset core
written for this lab (`rtl/psp_mini.v`). You write the spec core and the miter.
Everything is built with yosys and its bundled tools, and iverilog is used for
simulation.

> **Platform.** This lab is pure RTL simulation and formal verification. Nothing
> in it depends on timing, so it behaves the same on x86_64 and aarch64 (Docker
> on Apple silicon). Every part is "sim/formal"; there is no native backend.
>
> **Spoilers.** `solutions/` contains the reference `spec_core.v` and `miter.v`.
> Don't open them until you are done.

## Learning goals

* Turn an informal ISA description into an executable, single-cycle **spec**,
  and learn why the spec is the part you have to trust.
* Build a **miter** that compares a multi-cycle implementation with the spec at
  instruction granularity (commit points). Know where to compare and why.
* Run **bounded model checking (BMC)**, read a counterexample trace, and turn it
  into a root cause.
* Check your verification environment itself: sanity bugs, mutants, vacuity.
* Know what a bounded proof does and does not give you, and compare formal
  verification with the fuzzing from lab 6.

## Mapping to the lectures

This lab goes with the lecture on formal verification of hardware (L13). It uses
specs and refinement, miters and equivalence checking, BMC and SAT/SMT solving,
counterexamples, and the limits of bounded proofs. It reuses lab 6's threat
model, where the CPU itself is untrusted (ALU errata, backdoors / hardware
Trojans), and Discussion 4-3 compares the two approaches directly.

## Background

### Implementation, specification, refinement

An ISA spec describes a processor as a state machine at the *architectural*
level. The state is the pc, the registers, memory, the privilege level and the
CSRs. Each instruction is one atomic step `S' = step(S, insn)`. A real
implementation spends several cycles per instruction (or overlaps many of them)
and has lots of extra *micro-architectural* state: instruction registers,
operand latches, pipeline registers, caches.

The implementation is correct if it **refines** the spec. Look only at
architectural state, and only at the moments when an instruction has fully
finished (its *commit* or *retirement* point). Then every behaviour of the
implementation must be a behaviour the spec allows. psp-mini makes this easy to
state. It takes 4 cycles per instruction and changes architectural state only at
the clock edge that ends the commit cycle. So "after the n-th commit,
`impl.arch_state == spec.arch_state` after n spec steps" is the whole property.

### Miters

A miter is a circuit that holds both designs, feeds them the same inputs, and
raises a flag when their outputs differ. The question "are these equivalent?"
then becomes "can the flag ever go high?", which a solver can attack. Here the
two designs run at different speeds, so the miter also has to line them up in
time. It steps the spec exactly when the implementation commits, and compares
states at the next instruction boundary:

```
            free input `instr` (any 32-bit word, chosen by the solver every cycle)
                           |
 +-------------------------|------------------------------------------------------+
 | miter                   v                                                      |
 |  +-----------------------------------+      latched while imem_en (FETCH):     |
 |  | psp_mini  (implementation)        |----> f_insn <= instr, f_pc <= pc        |
 |  | FETCH -> DECODE -> EXEC -> WB     |                 |                       |
 |  | pc, x0..x3, mem, CSRs, priv       |                 | insn                  |
 |  +--+------------------------+-------+                 v                       |
 |     | commit (in WB)         | dbg_* (arch state) +-------------------------+  |
 |     +------- step ---------- | -----------------> | spec_core               |  |
 |                              +-- sync: copy ----> | one instruction / step  |  |
 |                              |   at FETCH         +------------+------------+  |
 |                              v                                 v dbg_*         |
 |   at FETCH : assert( impl arch state == spec arch state )      refinement      |
 |   at commit: assert( commit_insn == f_insn && commit_pc == spec pc )           |
 |   always   : assert( cycles since the last commit < 8 )        liveness        |
 +--------------------------------------------------------------------------------+
```

The instruction port is a **free input**. The solver may put any word on it in
any cycle, so the miter covers every program at once, including ones no
compiler would emit, and every encoding of every instruction field.

### Bounded model checking

Unroll the miter for K clock cycles: K copies of the combinational logic, with
copy t's flip-flop outputs wired to copy t-1's flip-flop inputs, and copy 1
starting from the reset state. Then ask a SAT solver whether there are input
values (instruction words, here) for which some assertion fails in some cycle
<= K. **SAT** means a concrete counterexample: the solver's assignment *is* a
program plus a waveform. **UNSAT** means no failing trace of length <= K exists.
That is a proof, but only up to K. The `bmc3` engine we use answers K
questions, one per depth: "can the assertion fail exactly at step k, given that
it held at every earlier step?" It reports the shortest counterexample.

Our default engine is ABC (`yosys-abc`, installed with yosys). yosys writes the
miter as an and-inverter graph (AIGER). ABC structurally hashes each unrolled
time frame and propagates constants before it calls its SAT solver. yosys also
has a simpler built-in `sat` command (`ENGINE=sat`). It asks exactly the same
question, but in this lab it is 10-25x slower once the trace holds more than a
couple of instructions (see "Measured runtimes"). Comparing the two is part of
Part 4.

### Re-anchoring (`sync`)

The provided wiring overwrites the spec's state with the implementation's state
at every instruction boundary, right after the comparison. Whenever the
comparison passed, the two states were already equal, so the copy changes
nothing. The *first* failing comparison of any trace is therefore identical with
and without it, which is all BMC reports. What it buys is solver performance.
Without it, the solver has to re-prove, through every earlier instruction, that
the spec's copy of x1 is equal to the implementation's. With it, both machines
start each instruction from literally the same variables. You will measure the
difference (`NOSYNC=1`) and explain it in Discussion 4-2.

## The psp-mini ISA (your spec implements exactly this)

psp-mini is RV32I-flavoured, with simplifications that keep the formal problems
small. `rtl/psp_defs.vh` holds the constants.

**State.** `pc` (32 bits), `priv` (U=0, M=1), registers `x0..x3` (32 bits; `x0`
always reads 0 and writes to it are dropped), data memory `mem[0..3]` (32-bit
words), CSRs `mtvec`, `mepc` (32 bits), `mcause` (4 bits), `mstatus.MPP` (1 bit:
U or M).

**Reset.** `pc=0`, `priv=M`, all registers, memory words and CSRs 0, `MPP=U`.

**Quirks (deliberate):**
* Only bits [1:0] of each 5-bit register specifier are decoded: `x5` *is*
  `x1`, and `x4` is `x0`.
* Memory word index = address bits [3:2]. All other address bits are ignored,
  so the 16-byte memory repeats across the address space. There are no
  misaligned-access exceptions.
* Every new pc (jump/branch target, `mtvec`, `mepc`) has bits [1:0] forced to
  00. There is no instruction-address-misaligned exception.

| instruction | encoding (must match exactly) | effect (then `pc += 4` unless stated) |
|---|---|---|
| `LUI rd, imm` | opcode `0110111` | `x[rd] = imm_u` |
| `JAL rd, off` | opcode `1101111` | `x[rd] = pc+4; pc = pc + imm_j` |
| `BEQ rs1, rs2, off` | `1100011`, funct3 `000` | `if x[rs1]==x[rs2]: pc = pc + imm_b` |
| `LW rd, off(rs1)` | `0000011`, funct3 `010` | `x[rd] = mem[(x[rs1]+imm_i)[3:2]]` |
| `SW rs2, off(rs1)` | `0100011`, funct3 `010` | `mem[(x[rs1]+imm_s)[3:2]] = x[rs2]` |
| `ADDI rd, rs1, imm` | `0010011`, funct3 `000` | `x[rd] = x[rs1] + imm_i` |
| `SRLI rd, rs1, sh` | `0010011`, funct3 `101`, funct7 `0000000` | `x[rd] = x[rs1] >> insn[24:20]` (logical) |
| `ADD rd, rs1, rs2` | `0110011`, funct3 `000`, funct7 `0000000` | `x[rd] = x[rs1] + x[rs2]` |
| `XOR rd, rs1, rs2` | `0110011`, funct3 `100`, funct7 `0000000` | `x[rd] = x[rs1] ^ x[rs2]` |
| `ECALL` | exactly `0x00000073` | trap, cause 8 (from U) or 11 (from M) |
| `MRET` | exactly `0x30200073`, **M-mode only** | `pc = mepc; priv = MPP; MPP = U` |
| `CSRRW rd, csr, rs1` | `1110011`, funct3 `001`, **M-mode only**, csr in the table below | `t = csr; csr = legalize(x[rs1]); x[rd] = t` |

Immediates are the standard RV32I ones (`imm_i`, `imm_s`, `imm_b`, `imm_u`,
`imm_j` in `src/spec_core.v`). Arithmetic wraps modulo 2^32.

| CSR | address | read | write (`legalize`) |
|---|---|---|---|
| `mstatus` | `0x300` | MPP in bits [12:11] (`11` = M, `00` = U), all other bits 0 | MPP = M iff bits [12:11] == `11`, else U |
| `mtvec` | `0x305` | value | bits [1:0] forced to 0 |
| `mepc` | `0x341` | value | bits [1:0] forced to 0 |
| `mcause` | `0x342` | zero-extended 4 bits | keeps bits [3:0] |

**Illegal instructions.** Anything not matched above is illegal and traps with
cause 2. That includes other funct3/funct7 values, other SYSTEM encodings, the
whole `custom-0` opcode space (`0001011`), MRET or CSRRW from U-mode, and CSRRW
to any other CSR address.

**Traps** (illegal instruction or ECALL). No register or memory write takes
place. `mepc = pc` (the trapping instruction), `mcause = cause`,
`MPP = priv`, `priv = M`, `pc = mtvec`. A trapping instruction still *commits*
(it is one retirement on the commit port).

## The implementation and the files

```
rtl/psp_defs.vh        shared constants (opcodes, CSR numbers, causes, sizes)
rtl/psp_mini.v         the DUV: 4-state multi-cycle core.  Defines:
                         PSP_GLITCH    defect 1 (ALU)      PSP_BACKDOOR  defect 2
                         PSP_MUTANT=n  known simple bug n (see "Mutants")
build/defects.vh       magic constants of the defects, generated per SEED by `make`
src/spec_core.v        YOUR spec (skeleton: ADDI done)
src/miter.v            YOUR miter (skeleton: wiring, reset, sync, one assertion)
solutions/             reference spec_core.v and miter.v (spoilers)
sim/tb.v               iverilog testbench: program ROM + impl/spec lock-step
programs/*.s           directed test programs with '#!' expectations
tools/rvasm.py         assembler/disassembler for psp-mini
tools/sim.py           `make sim` driver
tools/bmc.py           `make bmc` driver (yosys -> ABC bmc3 -> yosys sim replay)
tools/cex_dump.py      `make cex`: counterexample VCD -> readable trace
check.py               the grader
```

The implementation's interface (`imem_*`, `commit*`, `dbg_*`) is what your miter
can see. Treat the inside of `psp_mini.v` as vendor IP. You may read it, but
find the defects with the solver, not by code review. Their constants are
scrambled per seed anyway.

## Parts

Time estimates assume you know Verilog and have read the background.

### Part 1: ALU spec and a first miter (about 2.5 h)

Restrict the instruction stream to the five ALU instructions (`ALU_ONLY=1`, an
`assume` in the skeleton). Get a working, non-vacuous miter for them.

* **Exercise 1-1.** Implement LUI, ADD, XOR and SRLI in `src/spec_core.v`. Check
  with `make sim PROG=programs/alu.s`: expectations ok *and* co-sim ok. (The
  other programs keep failing co-sim until Part 3.)
* **Exercise 1-2.** Complete `src/miter.v`. Latch the fetched word and pc, step
  the spec at commit, assert the commit-time checks, and assert pc, registers
  and data memory at every instruction boundary. Then
  `make bmc ALU_ONLY=1 K=18` must report PASS.
* **Exercise 1-3.** Sanity-check the environment. First,
  `make bmc ALU_ONLY=1 K=18 MUTANT=1` (XOR is implemented as OR) must report a
  counterexample, and so must mutants 2-4. Second, plant a bug of your own in
  your *spec* (e.g. make ADD subtract), watch BMC catch it, then revert. Also
  run `make sim MUTANT=1`. The directed tests pass!
* **Discussion 1-1.** Why do the directed programs miss mutant 1 while BMC finds
  it? What was the shortest counterexample, and how does its length follow from
  the micro-architecture?
* **Discussion 1-2.** What exactly does `ALU_ONLY` assume? What could go wrong
  if an assumption (or the reset logic) were too strong, and how did you
  convince yourself that a PASS means something?

### Part 2: find the ALU glitch (about 1 h)

* **Exercise 2-1.** `make bmc ALU_ONLY=1 DEFECTS=glitch K=18`, then `make cex`.
  From the trace, work out the condition under which the adder is wrong (which
  operand values) and what goes wrong (which bit). Write your seed (from
  `build/defects.json`) and your findings in `report.md`. Re-run with
  `make newseed` to see that the defect moves.
* **Exercise 2-2.** Find the smallest K that exposes the glitch (try K = 6, 10,
  14) and explain it in terms of how many instructions the solver needs to
  *build* the magic operand from the all-zero reset state.
* **Exercise 2-3.** Turn the counterexample into a regression test. Write
  `programs/glitch.s` that triggers the glitch, with `#!` expectations giving
  the *architecturally correct* result. `make sim PROG=programs/glitch.s` passes,
  while `make sim PROG=programs/glitch.s DEFECTS=glitch` fails. (The grader runs
  all programs with the defects off, so the expectations must be correct.)
* **Discussion 2-1.** Estimate how many random ADD/ADDI executions a fuzzer
  (lab 6) would need to hit this glitch by chance. Why does BMC find it in
  seconds?
* **Discussion 2-2.** What does one counterexample *not* tell you about a
  defect? How would you use the solver to learn more? (Hint: add assumptions
  that exclude the trigger you found and run again.)

### Part 3: the whole ISA, exceptions, and the backdoor (about 3 h)

* **Exercise 3-1.** Finish the spec: JAL, BEQ, LW, SW, CSRRW with the CSR read
  mux, ECALL, MRET, illegal instructions and traps. `make sim` must be clean for
  all programs.
* **Exercise 3-2.** Extend the miter with privilege and the four CSRs at the
  boundary, plus the bounded-liveness check (fewer than 8 cycles between
  commits). `make bmc K=14` (no `ALU_ONLY`) must PASS.
* **Exercise 3-3.** Run mutants 5-14 (`make bmc MUTANT=n K=18`). All must be
  caught. Which assertion catches mutant 13, and which catches mutant 14? Why
  would a miter that only compares registers miss them?
* **Exercise 3-4.** `make bmc DEFECTS=backdoor K=18`, then `make cex`. Report
  the backdoor encoding (opcode, funct3, funct7), which register must hold which
  value, what the instruction does, and why that is a security problem.
* **Discussion 3-1.** The backdoor only fires with a 32-bit key in a register.
  Lab 6's opcode scan can tell you that an undocumented encoding exists, but a
  test-based search for the key would have to try up to 2^32 values. Compare that
  with how BMC copes with the key. Then
  suppose you cannot change the RTL: what run-time defense would stop this
  backdoor (for example a monitor that flags any U-to-M privilege change that
  is not a trap), and what would it cost?
* **Discussion 3-2.** The spec is now your trusted base. Give one concrete
  mistake in a spec that would hide a real implementation bug, even with a
  perfect miter and solver. How can a spec be validated?

### Part 4: bounded safety (about 1.5 h)

* **Exercise 4-1.** With both defects off, `make bmc K=30` (7 instructions).
  Record the runtime, then raise K (e.g. 46, 66, 102) and tabulate depth against
  runtime. State the largest K you proved and say precisely what the result
  means.
* **Exercise 4-2.** Repeat a few depths with `NOSYNC=1` and with `ENGINE=sat`
  (keep K <= 14 for `sat`) and add them to the table.
* **Discussion 4-1.** What does "bounded" leave out? Describe a defect that
  psp-mini could contain but that no BMC run with your K would find. How would
  k-induction or an invariant close the gap?
* **Discussion 4-2.** Argue that re-anchoring (`sync`) is sound for BMC (the
  first failing check is the same with and without it), and explain why it
  speeds the solver up. When would it be *unsound* to use?
* **Discussion 4-3.** Compare fuzzing (lab 6) and formal verification (this
  lab): what each finds, what each assumes, how each scales with design size
  and bug depth, and how you would combine them on a real core.

## Build, run, check

```
make                      # place defects (random seed), assemble, compile, parse
make sim                  # all programs, co-simulated against src/spec_core.v
make sim PROG=programs/alu.s TRACE=1
make bmc K=18 ALU_ONLY=1  # BMC of src/miter.v + src/spec_core.v
make bmc K=18 DEFECTS="glitch backdoor"
make bmc K=14 MUTANT=7
make cex                  # readable trace of the last counterexample
make check                # grade your files (fresh seed every run; SEED=n to repeat)
make check-solution       # grade the reference solution
make newseed              # move the defects; `make SEED=n` restores a seed
```

`make bmc` exits with status 1 when it finds a counterexample, so make prints
`Error 1`. That is expected. Each run leaves `build/bmc/` with `bmc.ys`
(the yosys script), `bmc.log`, `cex.vcd` (open it in GTKWave or any VCD viewer)
and `result.json`. Step 1 is the reset cycle. Instruction n commits in step
4n+1 and is checked in step 4n+2, so K = 4n+2 covers n instructions.

## Hints

* Start from the timeline in the miter skeleton. The boundary check happens in
  the FETCH cycle, which is also when `instr` is sampled.
* "Assertion fails at step 2" almost always means the reset states differ or
  the spec's state isn't driven. "Fails at step 5" is a commit-time check.
* In `always @*` blocks, assign every variable before you read it. Reading
  first makes iverilog and yosys disagree: simulation uses the stale value,
  synthesis the final one.
* If `make sim` co-sim is clean but BMC fails, the counterexample uses
  something your programs don't: odd encodings (`x5`, funct7 bits, upper bits of
  the CSR field), U-mode, or traps inside traps. Read the disassembly in
  `make cex`.
* If BMC passes suspiciously fast or never fails, try a mutant. A miter that
  checks nothing passes everything.
* Instructions in a counterexample often look strange (`addi x29(=x1), ...`).
  The solver uses any encoding it likes.

## Mutants

`make bmc MUTANT=n` (and `make sim MUTANT=n`) plants one simple, known bug in
the implementation. A good miter catches every one. The grader checks that
yours does.

| n | bug | n | bug |
|---|---|---|---|
| 1 | XOR computes OR | 8 | MRET does not reset MPP to U |
| 2 | SRLI shifts arithmetically | 9 | `mtvec` writable from U-mode |
| 3 | ADDI immediate not sign-extended | 10 | ECALL from U reports cause 11 |
| 4 | `x0` is writable | 11 | JAL links `pc` instead of `pc+4` |
| 5 | BEQ compares only 16 bits | 12 | custom-0 executes as a NOP |
| 6 | LW ignores its offset | 13 | MRET hangs the core (never commits) |
| 7 | traps save `pc+4` in `mepc` | 14 | commit port reports the wrong pc for BEQ |

## What `make check` does

It places the defects with a fresh seed and then checks the following:

* **Part 1 (25 pts).** `alu.s` co-sim is clean. ALU-only BMC at K=18 passes.
  Mutants 1-4 are caught.
* **Part 2 (20 pts).** ALU-only BMC with the glitch finds a counterexample, and
  that trace really feeds the magic operands to the adder.
* **Part 3 (35 pts).** All programs co-sim cleanly. Full-ISA BMC at K=14 passes.
  The backdoor counterexample really executes the backdoor encoding with the
  key in rs1. Mutants 5-14 are caught.
* **Part 4 (20 pts).** BMC at K=30 with both defects off passes.

`check-solution` takes about 65 s on 2 CPUs. Discussion answers go in
`report.md` and are not auto-graded.

## Measured runtimes and the choices behind them

Measured with the reference solution on the 2-CPU x86_64 build box (yosys 0.33).
The times include about 4 s of yosys front end per run.

| run | result | time |
|---|---|---|
| ALU-only, no defects, K=18 | pass | 7 s |
| ALU-only, glitch, K=18 | cex at step 14 | 5 s |
| full ISA, no defects, K=14 | pass | 6.5 s |
| full ISA, backdoor, K=18 | cex at step 14 | 5.5 s |
| full ISA, no defects, K=30 / 46 / 66 / 102 | pass | 13 / 21 / 46 / 101 s |
| same with `NOSYNC=1`, K=14 / 18 / 22 / 30 | pass | 17.5 / 32 / 54 / 77 s |
| `ENGINE=sat` (yosys MiniSAT), K=6 / 10 / 14 | pass | 3 / 37 / 159 s |
| `ENGINE=sat`, ALU-only glitch, K=14 | cex at step 14 | 32 s |
| unbounded attempt with ABC `pdr` (not provided) | undecided after 140 s | |

Each of the following choices keeps every check in the grader below 20 s:

* **4 registers and 4 memory words.** With 8 registers the glitch search took
  46 s instead of 0.6 s of solver time, and the ALU proof at K=18 took 17 s
  instead of 3 s.
* **Re-anchoring** (see above): at K=30 it makes a 6x difference.
* **Explicit `x0` read multiplexers** in both the implementation and the spec.
  An implementation that relies on "`rf[0]` is never written" forces the solver
  to re-derive that invariant through every earlier cycle. In early experiments
  this alone turned a 0.4 s check into a time-out.
* **ABC `bmc3` instead of `sat`.** Same question, much faster solver pipeline.

## Going further

* **Unbounded proofs.** `sat -tempinduct` (k-induction) fails out of the box,
  because the induction step may start in an unreachable state (e.g. the
  implementation in EXEC with garbage operands). Add strengthening assertions
  (`ir == f_insn` while not in FETCH, architectural states equal in *every*
  cycle, and so on) until the induction step goes through. ABC's `pdr` (IC3)
  does not converge on this miter within minutes. Why is a bit-level invariant
  for "two copies of 32-bit datapaths are equal" hard for it?
* **Pipelines.** Add a 2-stage pipeline with forwarding to `psp_mini.v`. The
  commit-point miter still works unchanged. That is the idea behind
  riscv-formal's RVFI interface and Arm's ISA-Formal.
* **Information flow.** Verify a security property instead of functional
  equivalence. Example: "U-mode code cannot influence mtvec", by
  self-composition (two copies of the core that differ only in U-mode inputs).
* **Coverage.** Add `cover` statements, or BMC with an inverted assertion, to
  prove that interesting states (U-mode, a taken branch inside a trap handler)
  are reachable within K. This is the formal answer to "is my PASS vacuous?".

## References

* A. Biere, A. Cimatti, E. Clarke, Y. Zhu. *Symbolic Model Checking without
  BDDs.* TACAS 1999. (BMC)
* J. Burch, D. Dill. *Automatic Verification of Pipelined Microprocessor
  Control.* CAV 1994. (Refinement against an ISA spec, flushing)
* M. Sheeran, S. Singh, G. Stålmarck. *Checking Safety Properties Using
  Induction and a SAT-Solver.* FMCAD 2000. (k-induction)
* A. Bradley. *SAT-Based Model Checking without Unrolling.* VMCAI 2011.
  (IC3/PDR)
* R. Brayton, A. Mishchenko. *ABC: An Academic Industrial-Strength
  Verification Tool.* CAV 2010.
* N. Eén, N. Sörensson. *An Extensible SAT-solver.* SAT 2003. (MiniSAT, used
  by yosys `sat`)
* A. Reid et al. *End-to-End Verification of ARM Processors with ISA-Formal.*
  CAV 2016.
* C. Wolf. *riscv-formal* and the RISC-V Formal Interface (RVFI).
  github.com/YosysHQ/riscv-formal
* R. Zhang, C. Deutschbein, P. Huang, C. Sturton. *End-to-End Automated Exploit
  Generation for Validating the Security of Processor Designs.* MICRO 2018.
* M. Hicks, M. Finnicum, S. King, M. Martin, J. Smith. *Overcoming an Untrusted
  Computing Base: Detecting and Removing Malicious Hardware Automatically.*
  IEEE S&P 2010.
* R. Kande et al. *TheHuzz: Instruction Fuzzing of Processors Using
  Golden-Reference Models for Finding Software-Exploitable Vulnerabilities.*
  USENIX Security 2022.
