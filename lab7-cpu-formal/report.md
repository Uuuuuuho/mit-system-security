# Lab 7 report: formal verification of psp-mini

Name:
Defect seed used for Parts 2-3 (from `build/defects.json`):

## Part 1: ALU spec and first miter

### Discussion 1-1: directed tests vs. BMC on mutant 1

<!-- Why do programs/*.s miss mutant 1 while BMC catches it? Length of the
     shortest counterexample, and how it follows from the micro-architecture. -->

### Discussion 1-2: assumptions and vacuity

<!-- What exactly does ALU_ONLY assume? What goes wrong if an assumption or the
     reset logic is too strong? How do you know your PASS is meaningful? -->

## Part 2: the ALU glitch

Findings (Exercise 2-1):

| item | value |
|---|---|
| trigger condition on operand a | |
| trigger condition on operand b | |
| effect on the result | |
| shortest K that exposes it (Exercise 2-2) | |

### Discussion 2-1: how likely is a fuzzer to hit it?

<!-- Expected number of random ADD/ADDI executions until the glitch fires.
     Why BMC needs seconds. -->

### Discussion 2-2: what one counterexample does not tell you

<!-- And how you would use the solver to learn more. -->

## Part 3: whole ISA, exceptions, backdoor

Findings (Exercise 3-4):

| item | value |
|---|---|
| encoding (opcode / funct3 / funct7) | |
| register and key value required | |
| effect | |
| mutant 13 caught by | |
| mutant 14 caught by | |

### Discussion 3-1: the key, lab 6 vs. BMC, and a run-time defense

### Discussion 3-2: the spec as trusted base

<!-- One concrete spec mistake that hides a real bug; how to validate a spec. -->

## Part 4: bounded safety

Runtime table (Exercises 4-1 and 4-2):

| K | instructions | engine | sync | result | time (s) |
|---|---|---|---|---|---|
| 30 | 7 | abc | yes | | |
| | | | | | |

Largest K proved, and what exactly it means:

### Discussion 4-1: what "bounded" leaves out

### Discussion 4-2: why re-anchoring is sound and why it is fast

### Discussion 4-3: fuzzing (lab 6) vs. formal verification (lab 7)
