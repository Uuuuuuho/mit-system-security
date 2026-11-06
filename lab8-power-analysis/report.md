# Lab 8 report: physical side channels on synthetic traces

Name:
Seed(s) used for the numbers below (`SEED=...`):
`make check` score:

## Discussion 1-1: what square-and-multiply leaks

*The implementation property you exploited, how you segmented/decoded the trace, and the timing-channel
equivalent (Kocher 1996). Would equal-length squaring and multiplication help?*

## Discussion 1-2: alignment and averaging

*How the noise of the average scales with the number of traces; the noise level at which your Exercise 1-2
decoder fails with 25 traces (show how you measured it); which blinding countermeasure stops averaging.*

## Discussion 1-3: residual leakage of the Montgomery ladder

*Two ways a regular ladder can still leak (register/address, operand Hamming distance, caches); cost and benefit
of exponent blinding.*

## Discussion 2-1: difference-of-means peaks

*Where the peak of the right guess appears (tags) and why at several samples; measured height versus the model;
a ghost peak of a wrong guess; S-box output versus AddRoundKey output as the target.*

## Discussion 3-1: CPA versus DPA

| attack | traces needed (your measurement) | notes |
|---|---|---|
| single-bit DoM | | |
| multi-bit DoM | | |
| CPA (HW) | | |

*Relate the CPA number to N ~ 28 / rho^2 with rho computed from sigma.*

## Discussion 3-2: choosing the leakage model

*Why the HW attack fails on the HD device; the sample that only leaks HW(k_i) and why it is useless to CPA; one
real situation for each model.*

## Discussion 3-3: rank curves

*Plot or table for sigma = 1, 2, 4; scaling of traces-needed with sigma; comparison with the rule of thumb; why one
curve is not a security claim.*

## Discussion 4-1: your masking scheme

*Masks and where they are refreshed or converted; the MixColumns pitfall; cost versus the unmasked firmware
(instructions, random bytes per encryption, RAM).*

| | unmasked | masked |
|---|---|---|
| instructions per encryption | 1484 | |
| random bytes per encryption | 0 | |
| TVLA max abs t (HW) | | |

## Discussion 4-2: masking under the Hamming-distance model

*Output of `tvla --model hd` (summarized), explanation of the leaking samples, two fixes and their cost, and why
glitches matter in hardware.*

## Discussion 4-3: second-order attacks

*Derivation of E[(HW(y ^ m) - 4)(HW(m) - 4)] = -(HW(y) - 4) / 2; trace count versus sigma for second versus first
order; implications for d-th order masking. Traces your Exercise 4-2 attack needed (if attempted).*

## Discussion 4-4: hiding countermeasures

*Correlation and traces-needed factors for shuffling (plain CPA versus your attack); what happens with
`shuffle="sbox"`; per-instruction delays and alignment techniques; cost to the defender.*

## Discussion 4-5: frequency and power channels from software

*(a) the mechanism and the knee in `powersim/dvfs.py`; (b) what the Hertzbleed attacker controlled and why;
(c) PLATYPUS and CLKSCREW: what the interface got wrong, the mitigations and their cost; link to the HW/HD
models.*
