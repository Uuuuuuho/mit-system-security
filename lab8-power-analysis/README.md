# Lab 8: Physical side channels on synthetic power traces (`powersim`)

A chip's supply current depends on what it computes. In this lab you take secrets out of
power traces: first by reading them off a single trace (SPA), then statistically from thousands
of traces (DPA, CPA). After that you build and break the standard countermeasures: Boolean
masking, random delays and shuffling. The traces come from `powersim`, a small numpy simulator
in which every instruction of a toy 8-bit firmware becomes one power sample. That gives you
exactly what the bench never does: a known leakage model, a knob for the noise, and a listing
of what executes at every sample.

**Simulator only.** There is no native backend (`native_backend: none`). Real power traces need
an oscilloscope or a ChipWhisperer-class capture board plus a target board, which the lab
cannot assume. Everything is deterministic for a given `SEED`, needs only Python 3 + numpy
(matplotlib is optional), and behaves the same on x86_64 and aarch64, including Docker on
Apple silicon.

| Part | What you do | Mode | Time |
|---|---|---|---|
| 1 | SPA: read an RSA-style exponent off square-and-multiply traces; make it regular | sim | ~2 h |
| 2 | DPA: difference of means on the first-round AES S-box output | sim | ~1.5 h |
| 3 | CPA: Pearson correlation, HW versus HD model, key rank versus number of traces | sim | ~2.5 h |
| 4 | Countermeasures: first-order masking (+ optional second-order attack), random delays, shuffling; frequency/power channels | sim | ~5 h |

## Learning goals

* Explain where data-dependent power comes from in CMOS logic, and when the Hamming-weight or
  the Hamming-distance model is the right abstraction.
* Read control flow (and therefore secrets) from a single trace when the instruction sequence
  depends on a secret (SPA). Align and average traces when one trace is too noisy.
* Turn a leakage model into a statistical distinguisher: difference of means (Kocher et al.)
  and correlation (Brier et al.). Reason about how many traces an attack needs from the
  signal-to-noise ratio. Measure it with key-rank curves.
* Implement first-order Boolean masking for a whole AES-128, find its pitfalls (mask
  cancellation in MixColumns, re-masking between rounds, HD leakage), and verify it with a
  TVLA-style t-test. See why masking only raises the attack order (second-order CPA).
* Quantify what hiding countermeasures (random delays, shuffling) cost an attacker, and undo
  them (re-alignment by correlation, integration over the shuffled slots).
* Connect power side channels to software-visible channels: DVFS turns power into timing
  (Hertzbleed), energy counters expose power to software (PLATYPUS), and software-controlled
  voltage/frequency turns into fault injection (CLKSCREW).

**Lecture mapping.** This lab belongs to lecture L07 (physical side channels: power and EM
analysis, SPA/DPA/CPA, masking and hiding). Discussion 4-5 covers the frequency/power channels
reachable from software (CLKSCREW, Hertzbleed, PLATYPUS), which lead into L08.

## Quick start

```sh
make                      # syntax check + simulator selftest
make check                # grade your code in src/  (prints SEED; SEED=123 make check repeats a run)
make check PARTS=3        # grade one part
make check-solution       # grade the reference solution (solutions/ is full of spoilers!)
make show DEV=aes ARGS="--full --per-char 4"     # one whole AES trace as text: count the rounds
make show DEV=modexp ARGS="--bits 16 --noise 0.3" # a short exponentiation
python3 src/part3_cpa.py --curve --noises 1,2,4   # rank curves (PNG in out/ if matplotlib exists)
```

In Docker, use `./scripts/lab-make.sh -C lab8-power-analysis check` from the repository root.
A full grading run takes about 10 s for the reference solution. Keys, exponents, masks and noise
are randomized for each run, so hard-coded answers fail.

## The simulator

### Where the power goes (and why it depends on data)

Most of the energy a CMOS circuit uses is dynamic: every time a node switches from 0 to 1, its
load capacitance is charged from the supply, and on the 1-to-0 transition it is discharged.
Within one clock cycle the current drawn from the supply is therefore a sum over all wires that
toggle, weighted by their capacitance. Two things decide that sum:

1. **Which operation runs.** Fetching and decoding an opcode, activating the multiplier rather
   than the XOR unit, and reading flash rather than RAM all draw different amounts of current,
   whatever the data. This *instruction-dependent* part is large and makes the shape of a trace
   reveal the program's control flow. Simple power analysis (SPA) only needs this part.
2. **Which data moves.** Long, heavily loaded wires such as data buses, register-file ports and
   the outputs of wide registers dominate the data-dependent part. How many of their bits toggle
   depends on the values carried. This part is usually small, but it is correlated with
   secrets, and differential attacks exploit exactly this correlation.

The simulator abstracts both parts into one number per instruction:

```
sample = BASE[kind] + GAIN[kind] * L + noise,      noise ~ N(0, sigma^2), independent per sample
```

**Hamming-weight model, `model="hw"`: L = HW(v),** the number of one-bits in the value `v`
that the instruction produces. This is what you see when a bus is *precharged* to all zeros
before each transfer: each one-bit then has to be driven up, so the energy is proportional to
HW(v). Many microcontrollers behave close to this. HW also describes any value whose
predecessor on the same wires is a known constant.

**Hamming-distance model, `model="hd"`: L = HD(prev, v) = HW(prev XOR v),** the number of bits
that flip when register `reg` is overwritten. This is the more physical model for registers and
non-precharged buses, and the standard one for hardware AES, where a round register is
overwritten by the next round's state every cycle. The previous value matters: if it is
unrelated to the secret and random, HD(prev, v) tells you nothing about v. If it is the
S-box *input* and v is the S-box *output*, the flip count depends on both and on the key. In the
simulator each instruction names its destination register (the accumulator `r24` by default,
`bus` for stores), and a register's contents before its first write are random.

In both models only the instruction's **result** leaks: operands are assumed to be in registers
already. GAIN scales the data-dependent part. One bit of Hamming weight is one unit, and `sigma`
is given in the same unit. The instruction kinds (from `powersim/leakage.py`) are:

| kind | BASE | GAIN | used for |
|---|---|---|---|
| `nop` | 0.0 | 0 | idle cycles around the trigger |
| `load` / `store` | 4.0 / 3.0 | 1 | RAM byte load / store (stores overwrite register `bus`) |
| `xor` / `mov` / `xtime` | 2.0 / 2.5 / 3.5 | 1 | byte ALU operations |
| `lookup` | 6.0 | 1 | table lookup (S-box) |
| `rng` | 5.0 | 1 | reading a fresh random byte from the TRNG |
| `dummy` | random 2.0..6.0 | 1 | random-delay instructions and unrelated code (random data) |
| `loop` | 1.0 | 0 | loop bookkeeping between big-number operations |
| `sqr` / `mulrd` / `mul` | 6.0 / 4.5 / 6.5 | 0.25 | one 32-bit word written by squaring / fetched / written by multiplication |

**Signal, noise and trace counts.** For a uniformly random byte, Var(HW) = 2. A sample that
leaks HW(y) with GAIN 1 therefore has SNR = 2 / sigma^2. If your hypothesis matches the leakage
exactly, its correlation with that sample is rho = sqrt(SNR / (1 + SNR)). A useful rule of thumb
(Mangard 2004) for the number of traces that makes the correct guess stand out among 256 with high
confidence is about 28 / rho^2 when rho is small. Part 3 asks you to check it.

**Misalignment.** Real captures are misaligned in two ways, both modeled at render time:
*trigger jitter* (the scope triggers 0..J instructions early, and those samples show unrelated
code), and *random delays*, a countermeasure in which the firmware itself runs 0..D dummy
instructions at chosen points (`cpu.delay(D)`). Everything after a delay point shifts by a
different amount in every trace.

### The firmware is the listing

Victim firmware is plain Python in `powersim/firmware.py`. Each encryption runs on n inputs in
parallel (one numpy row each), and every data-processing step goes through a `Recorder` object
`cpu`, so one call is one instruction and one sample:

```python
a = cpu.load(st[:, i],        tag=f"r{r}.load[{i}]")    # load state byte i
x = cpu.xor(a, k[i],          tag=f"r{r}.ark[{i}]")     # AddRoundKey
y = cpu.lookup(SBOX, x,       tag=f"r{r}.sbox[{i}]")    # SubBytes
cpu.store(y,                  tag=f"r{r}.store[{i}]")
```

Read `aes_firmware`, `masked_sbox_firmware` and `modexp_sqm` before you start. They are short,
and they are what an attacker with the firmware binary would know. AES devices capture samples
0..199 by default:

```
0..7      trigger (8 nop)
8..71     round 1 AddRoundKey+SubBytes: byte i at 8+4i (load), 9+4i (xor k_i), 10+4i (lookup), 11+4i (store)
72..83    round 1 ShiftRows (12 moves)          84..159  round 1 MixColumns (19 instructions per column)
160..     round 2 ...                          (a whole encryption is 1484 samples: --full)
```

`TraceSet.tags[s]` names the instruction at sample `s` when there is no misalignment, and
`ts.index("r1.sbox[3]")` returns 22. The modexp device has no tags, because they would give the
exponent away.

### API cheat sheet

```python
from powersim import AESDevice, MaskedSboxDevice, ModExpDevice, Recorder, SBOX, HW8, expand_key
dev = AESDevice(seed=1, noise=2.0, model="hw")    # also: jitter=, round_delay=, shuffle=, window=(0, 200) | None
ts  = dev.capture(1000)                           # ts.traces (1000, 200) float32, ts.pts / ts.cts (1000, 16) uint8
ts[:100]                                          # first 100 traces, same tags
dev = ModExpDevice(seed=1, noise=1.0)             # ts.inputs (bases), dev.nbits = 128, dev.nwords = 8, dev.modulus
cpu = Recorder(n, "hw")                           # write your own firmware (Exercises 1-3 and 4-1)
cpu.load/mov/xor/xtime/lookup/store(...); cpu.random_bytes(k); cpu.masked_table(SBOX, m_in, m_out)
cpu.loop(); cpu.modmul(a, b, mod, nwords, reg=); cpu.modsqr(a, mod, nwords, reg=); cpu.delay(D)
cpu.render(noise=1.0, jitter=0, window=None)      # -> (n, samples) float32;  cpu.kinds, cpu.tags
from powersim.stats import welch_t, moving_average, sparkline, save_plot
```

Attributes that start with an underscore (`dev._key`, `dev._e`) are the secrets. The grader
reads them. Your attack code must not (the `__main__` blocks use them only to print whether you
were right).

---------------------------------------------------------------------------------------------------

## Part 1: Simple power analysis of modular exponentiation (~2 h, `src/part1_spa.py`)

`ModExpDevice` computes `base^e mod m` with left-to-right square-and-multiply. `e` is a secret
128-bit exponent with its top bit set, and `m` is a public 256-bit modulus. Squaring and
multiplication are different code paths: a square writes W = 8 result words, while a multiply
fetches W operand words (`mulrd`) and writes W result words. Each loop iteration starts with 3
quiet `loop` samples. Run `make show DEV=modexp ARGS="--bits 16 --noise 0.3"` and you will see
the exponent.

**Exercise 1-1.** Implement `recover_exponent(trace, nbits, nwords)`. You get one aligned trace
with noise sigma = 1, and the trigger nops are samples 0..15. Return all 128 bits.

**Exercise 1-2.** Implement `recover_exponent_multi(traces, nbits, nwords)`. You get 25 traces
of the same exponent (random bases) with noise sigma = 8 and trigger jitter 0..300. No single
trace can be decoded reliably. Align the traces by cross-correlation, average them, find the
start, and decode.

**Exercise 1-3 (defense).** Replace the starter's square-and-multiply in `ladder()` with a
Montgomery ladder whose instruction sequence does not depend on `e`. The grader checks the
results against `pow()` and compares the instruction sequences of three random exponents.

*Hints.* Look before you code: print `sparkline` views or save a PNG. Average a few samples per
decision. For 1-2, `np.correlate(x - x.mean(), ref - ref.mean(), "full")` or an FFT gives every
lag at once. The first squaring computes 1*1, so its words have tiny Hamming weight. The trailing
16 nops look like the leading ones.

**Discussion 1-1.** Which property of the implementation did you exploit, and how does the same
property show up as a *timing* side channel (Kocher 1996)? Would it help if squaring and
multiplication took the same number of cycles?

**Discussion 1-2.** How does the noise of the average scale with the number of traces? Estimate
the noise level at which your 1-2 decoder starts to fail with 25 traces. Which countermeasure
(message blinding, exponent blinding) would stop averaging across traces, and which would not?

**Discussion 1-3.** Your ladder is regular at the instruction level. Name two ways it can still
leak (think of which register is written, the Hamming distance between operands, and caches),
and what exponent blinding (e + r * phi(m)) costs and buys.

## Part 2: Differential power analysis (~1.5 h, `src/part2_dpa.py`)

The AES device leaks HW with noise sigma = 2. For a guess g of key byte b, predict one bit of the
first-round S-box output, `D = bit_j(SBOX[p_b ^ g])`, and split the traces into two groups by D.
For the right guess the groups differ at every sample that handles that byte, by exactly GAIN
times one bit. For a wrong guess, D is nearly independent of the real intermediate and the
difference averages out.

**Exercise 2-1.** Implement `dom_traces(traces, pts, byte, bit)` returning the (256, S)
difference-of-means traces, without Python loops over guesses or samples.

**Exercise 2-2.** Implement `dpa_recover_key(traces, pts)`. The grader gives you 3000 traces and
wants all 16 bytes.

*Hints.* `D.T @ traces` is a (256, S) matrix of group sums. Plot `dom_traces` for the right and a
wrong guess. Combining the 8 selection bits is cheap insurance against ghost peaks.

**Discussion 2-1.** At which samples (tags) does the peak for the right guess appear, and why at
more than one? How tall is it, compared with what the leakage model predicts? Show a "ghost
peak" of a wrong guess and explain why the S-box output, not the AddRoundKey output `p ^ k`, is
the target (try both).

## Part 3: Correlation power analysis (~2.5 h, `src/part3_cpa.py`)

CPA replaces the binary split with a full leakage prediction: for every guess, predict L =
HW(SBOX[p ^ g]) for every trace, then compute the Pearson correlation with every sample. Using all
8 bits of the prediction, and correlation's built-in normalization, makes CPA need far fewer
traces than single-bit DPA.

**Exercise 3-1.** Implement `pearson(traces, hyps)`, a vectorized (G, S) correlation matrix.

**Exercise 3-2.** Implement `cpa_recover_key(traces, pts, "hw")`. The budget is 400 traces at
sigma = 3.

**Exercise 3-3.** The `AESDevice(model="hd")` leaks register transitions instead. First run your
HW attack on it (`python3 src/part3_cpa.py --model hd --noise 2 --n 600`) and watch it fail. Then
read `aes_firmware`, write `hyp_hd_sbox()`, and recover the key from 600 traces.

**Exercise 3-4.** Implement `rank_curve(traces, pts, key, ns)`, the rank of every correct key
byte after n traces for each n in `ns`, and `traces_needed(ranks, ns)`. The grader runs it on
1500 traces at sigma = 4 and compares with its own CPA. `python3 src/part3_cpa.py --curve`
prints the table and plots it if matplotlib is installed.

*Hints.* Center, multiply, normalize: `(H - H.mean(0)).T @ (T - T.mean(0))`. Pick the guess by
|rho|, because the sign depends on the polarity of the leakage. For the rank curve, the five
running sums make the cost one pass over the data. Restricting to the round-1 samples is also
allowed.

**Discussion 3-1.** Compare DPA and CPA on the same device. How many traces does each need
(`--n`), and why? Relate CPA's number to the rule of thumb N ~ 28 / rho^2 with rho computed from
sigma.

**Discussion 3-2.** Why does the HW attack fail on the HD device? In the HD firmware, which
sample leaks only HW(k_i), and why is that sample useless to CPA? Give one real-world situation
where each model is the right one.

**Discussion 3-3.** Include your rank-curve plot (or table) for sigma = 1, 2, 4
(`--noises 1,2,4`). How does traces-needed scale with sigma? Does it match the rule of thumb?
Why is one curve not enough to state "this device breaks with N traces"?

## Part 4: Countermeasures (~5 h, `src/part4_countermeasures.py`)

### Masking

First-order Boolean masking splits every sensitive value v into shares (v ^ m, m) with m fresh,
uniform and secret. As long as every single instruction handles a value that is masked by at
least one uniform mask independent of the data, each sample's distribution is the same for every
key and plaintext, so no first-order (single-sample) statistic, DPA or CPA, can work.

**Exercise 4-1.** Implement `masked_aes(cpu, p0, p1, rk)`, a complete masked AES-128 in the
Recorder instruction set. The plaintext arrives as shares (p0 ^ p1 = pt). Return ciphertext shares
(c0, c1). The starter is the unprotected firmware in that interface. It is correct and leaks
everywhere. Run `python3 src/part4_countermeasures.py tvla` to see the leaks, then remove them.
The grader checks:
* correctness (c0 ^ c1 = AES(pt));
* at least 1000 instructions and 160 table lookups (honour rule: every operation on state data
  goes through `cpu`, and computing an unmasked value outside `cpu` is cheating yourself);
* **TVLA**: a fixed-vs-random Welch t-test (HW model, 1500 traces per group, sigma = 0.5,
  repeated twice) must find no sample with |t| > 4.5 in both repetitions;
* the grader's first-order CPA on 2000 traces recovers at most 2 key bytes (chance level).

*Hints.* The classic plan (Herbst et al. 2006) uses six random bytes per encryption: m_in and
m_out for the masked table `cpu.masked_table(SBOX, m_in, m_out)`, and one mask per row for
MixColumns. Ask yourself what `a0 ^ a1` is when both bytes carry the same mask. MixColumns is
linear: `powersim.mix_column_masks()` gives the output masks. Re-mask by XORing a *mask
difference* (computed from masks only) into the data, never by removing one mask and then adding
another. The plaintext and the ciphertext must stay masked as well: TVLA sees them.

**Exercise 4-2 (optional, bonus).** Masking only raises the order of the attack. In
`MaskedSboxDevice` (`masked_sbox_firmware`), the mask m_out is read from the TRNG at one sample,
and S(p ^ k) ^ m_out is handled at others. Implement `second_order_attack(traces, pts, tags)`:
center both samples, multiply them (the *centered product*), and run CPA against HW(S(p ^ k)) on
the product. The grader gives you 4000 traces at sigma = 1. First-order CPA on the same traces
finds nothing. Return `None` to skip.

**Discussion 4-1.** Describe your masking scheme: which masks, where you re-mask, and the place
where a naive scheme leaks in MixColumns. What does it cost compared with the unmasked firmware
(instructions, random bytes per encryption, RAM for the table)?

**Discussion 4-2.** Run `python3 src/part4_countermeasures.py tvla --model hd`. Your masked
implementation leaks under the Hamming-distance model. Explain the leaking samples with
HD(a ^ m, b ^ m) = HW(a ^ b). Propose two fixes and their costs. Why do hardware
implementations also have to worry about glitches, which this simulator does not model?

**Discussion 4-3.** For the HW model, show that E[(HW(y ^ m) - 4)(HW(m) - 4)] = -(HW(y) - 4) / 2
for a uniform byte m, which is why the centered product works. How does the number of traces for
a second-order attack grow with sigma compared with first order? What does that imply for
d-th order masking plus noise? Answer this even if you skipped Exercise 4-2.

### Hiding: random delays and shuffling

Hiding does not remove the leakage. It moves it around in time, so a fixed sample no longer
always shows the same intermediate.

**Exercise 4-3.** The delayed device has trigger jitter 0..60 and inserts 0..40 dummy
instructions before every round (window 0..299, sigma = 1.5). Plain CPA on 1000 traces fails.
Implement `align(traces)` to return a re-aligned (n, L) array covering at least the round-1 S-box
loop, and `attack_delays()`. The grader runs its own CPA on your aligned traces and also checks
your attack. Both must recover the full key from 1000 traces.

**Exercise 4-4.** The shuffled device processes the 16 bytes of each S-box loop, and the
ShiftRows moves, in a fresh random order (sigma = 3, 3000 traces). The tags name loop *slots*.
Implement `attack_shuffled(traces, pts, tags)`.

*Hints (4-3).* The S-box loop has a very regular instruction pattern (BASE 4, 2, 6, 3 repeated 16
times). Correlate that pattern, or a template cut from one trace, against every offset. Then
build a cleaner template from the aligned traces and repeat. *Hints (4-4).* Each slot holds byte b
with probability 1/16. What happens to HW(y_b) if you add up all the slots?

**Discussion 4-4.** Quantify both hiding countermeasures. For shuffling, derive the correlation
of plain CPA and of your attack relative to the unprotected case, and the resulting
traces-needed factors. Then try `AESDevice(shuffle="sbox", noise=3.0)`, which shuffles only the
S-box loop, with plain CPA: what goes wrong for the defender? For delays, what changes if the
firmware inserts a delay before *every instruction*, and which alignment techniques exist for
that case? What do the countermeasures cost the defender?

### Beyond the probe: frequency and power from software

**Discussion 4-5.** Run `python3 -m powersim dvfs`. The workload is constant-time (same
instructions, same addresses), yet its wall-clock time depends on the Hamming weight of the
secret. (a) Explain the mechanism in the toy governor (`powersim/dvfs.py`) and why it has a knee.
(b) Hertzbleed (Wang et al. 2022) used the same effect to extract keys from constant-time SIKE
code remotely. What did the attacker have to control to create a large power contrast, and why
does that matter? (c) PLATYPUS (Lipp et al. 2021) read power directly from the RAPL energy
counters, and CLKSCREW (Tang et al. 2017) went the other way, driving the DVFS regulators from
software to inject faults into TrustZone. For each, state what the hardware/software interface
got wrong and how vendors mitigated it (and at what cost). Relate this back to the HW/HD models of
this lab.

---------------------------------------------------------------------------------------------------

## What `make check` verifies

| Item | Points | Check |
|---|---|---|
| 1-1 | 7 | exact 128-bit exponent from one trace (sigma 1) |
| 1-2 | 8 | exact exponent from 25 traces (sigma 8, jitter 0..300) |
| 1-3 | 5 | ladder results correct; identical instruction sequence for 3 exponents |
| 2-1 | 8 | `dom_traces` equals the grader's (max error < 1e-3) |
| 2-2 | 12 | full key by DPA, 3000 traces, sigma 2 |
| 3-1 | 8 | `pearson` equals the grader's (max error < 1e-4) |
| 3-2 | 8 | full key by CPA, 400 traces, sigma 3 |
| 3-3 | 6 | full key on the HD device, 600 traces, sigma 2 |
| 3-4 | 8 | rank curve sane, consistent with `traces_needed`, close to the grader's |
| 4-1 | 14 | masked AES: correct, TVLA-clean (HW), first-order CPA at chance |
| 4-2 | bonus | full key by second-order CPA (optional; `make check-solution` requires it) |
| 4-3 | 10 | full key after your re-alignment (grader's CPA) and from your attack |
| 4-4 | 6 | full key from the shuffled device, 3000 traces, sigma 3 |

`make check` exits 0 only when every non-bonus item passes. The reference solution passes
well inside every budget (for example with 250 of the 400 traces in 3-2), so the budgets leave
room for simpler attacks than the reference.

## Going further

* **Last-round attack.** Capture whole encryptions (`window=None`) of the HD device and attack
  the last round from the ciphertexts. Which register transition do you target?
* **Profiling.** Build Gaussian templates (Chari, Rao, Rohatgi 2002) on a device whose key you
  choose (`AESDevice(key=...)`), then attack a fresh device with as few traces as possible.
  Compare with CPA.
* **Full-key rank.** Byte ranks multiply into a full-key rank only roughly. Implement histogram
  rank estimation (Glowacz et al. 2015) and plot log2(full-key rank) versus traces.
* **Harder hiding.** Put `cpu.delay(3)` before every instruction of the S-box loop (copy
  `aes_firmware`) and try elastic alignment or integration windows.
* **Masking under HD.** Make your masked AES pass `tvla --model hd` (register precharging with
  random values, per-byte masks) and measure the cost.
* **Toy Hertzbleed.** Use `powersim.dvfs.dvfs_runtime` on noise-free AES traces with a fixed key
  and try a timing-only key-recovery attack. How many runs does it need?
* **Real hardware.** Everything here transfers to a ChipWhisperer capture of an 8-bit AES on a
  microcontroller, except that the noise is not Gaussian and the leakage model is unknown.

## References

* P. Kocher. Timing attacks on implementations of Diffie-Hellman, RSA, DSS, and other systems. CRYPTO 1996.
* P. Kocher, J. Jaffe, B. Jun. Differential power analysis. CRYPTO 1999.
* E. Brier, C. Clavier, F. Olivier. Correlation power analysis with a leakage model. CHES 2004.
* S. Mangard. Hardware countermeasures against DPA: a statistical analysis of their effectiveness. CT-RSA 2004.
* S. Mangard, E. Oswald, T. Popp. *Power Analysis Attacks: Revealing the Secrets of Smart Cards.* Springer, 2007.
* M. Joye, S.-M. Yen. The Montgomery powering ladder. CHES 2002.
* K. Itoh, T. Izu, M. Takenaka. Address-bit differential power analysis of cryptographic schemes OK-ECDH and OK-ECDSA. CHES 2002.
* T. S. Messerges. Using second-order power analysis to attack DPA resistant software. CHES 2000.
* C. Herbst, E. Oswald, S. Mangard. An AES smart card implementation resistant to power analysis attacks. ACNS 2006.
* E. Prouff, M. Rivain, R. Bevan. Statistical analysis of second order differential power analysis. IEEE Trans. Computers, 2009.
* C. Clavier, J.-S. Coron, N. Dabbous. Differential power analysis in the presence of hardware countermeasures. CHES 2000.
* G. Goodwill, B. Jun, J. Jaffe, P. Rohatgi. A testing methodology for side-channel resistance validation (TVLA). NIST NIAT workshop, 2011.
* S. Chari, J. R. Rao, P. Rohatgi. Template attacks. CHES 2002.
* C. Glowacz, V. Grosso, R. Poussier, J. Schüth, F.-X. Standaert. Simpler and more efficient rank estimation for side-channel security assessment. FSE 2015.
* R. Benadjila, E. Prouff, R. Strullu, E. Cagli, C. Dumas. Deep learning for side-channel analysis and introduction to ASCAD database. J. Cryptographic Engineering, 2020.
* A. Tang, S. Sethumadhavan, S. Stolfo. CLKSCREW: Exposing the perils of security-oblivious energy management. USENIX Security 2017.
* M. Lipp, A. Kogler, D. Oswald, M. Schwarz, C. Easdon, C. Canella, D. Gruss. PLATYPUS: Software-based power side-channel attacks on x86. IEEE S&P 2021.
* Y. Wang, R. Paccagnella, E. He, H. Shacham, C. Fletcher, D. Kohlbrenner. Hertzbleed: Turning power side-channel attacks into remote timing attacks on x86. USENIX Security 2022.
* K. Murdock, D. Oswald, F. D. Garcia, J. Van Bulck, D. Gruss, F. Piessens. Plundervolt: Software-based fault injection attacks against Intel SGX. IEEE S&P 2020.

## Files

```
powersim/        the simulator: leakage.py (Recorder, models), aes.py, firmware.py (victims), devices.py,
                 stats.py, dvfs.py, selftest.py; `python3 -m powersim --help`
src/             your code: part1_spa.py, part2_dpa.py, part3_cpa.py, part4_countermeasures.py
solutions/       reference solutions with the same file names (SPOILERS)
check.py         the grader;   report.md: template for the Discussion items
```
