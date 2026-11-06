# psp-lite — a small RV32I platform for the shd-labs

psp-lite is an instruction-set simulator (ISS) for a 32-bit RISC-V core (RV32I) with machine/user privilege, a few
CSRs, traps, a `custom-0` vendor opcode space and **seeded hidden defects**. It is the CPU of
[lab6-cpu-fuzz](../../lab6-cpu-fuzz/README.md). It is written in portable C11 (x86_64 and aarch64 hosts) and runs at
≈100–190 M instructions/s.

## Build
```
make                         # -> build/psp-lite   (seed for the defects remembered in build/.seed)
make SEED=1234               # choose the defect seed
make DEFECTS=0               # compile every defect out (golden-only CPU)
make examples                # SDK examples -> build/examples/*.elf|.bin|.dis
make smoke                   # build everything and run tests/smoke.py (24 checks)
make SHADOW_STACK_SRC=$PWD/my_ss.c   # optional exercise: your shadow-stack implementation (see below)
```
`make defects-dump` builds a grader-only tool that prints the defect parameters of a seed — using it while doing lab6
spoils the lab.

## Running programs
```
psp-lite [options] program.elf|program.bin
  --golden            disable every seeded defect (reference behaviour of the same binary)
  --trace             one line per retired instruction (and per trap) on stderr; --trace-file F to a file
  --dump-regs         register file at exit          --stats         instruction count and speed
  --max-instr N       stop after N instructions (exit 124)
  --input F           serial input from F (default stdin)
  --flag STR          contents of the M-mode-only flag CSRs 0xFC0–0xFC7
  --aslr | --aslr-seed N | --base ADDR    load the image at a random / reproducible / chosen 4 KiB-aligned base
  --ram-mb N          RAM size (default 64 MiB)  --flat          force flat-binary loading
  --shadow-stack      enable the shadow-stack hooks
```
Exit status: the program's exit code (written to CSR 0x802); `64 + mcause` for an unhandled trap (mtvec = 0);
124 when the instruction limit is hit; 2 on load errors.

## Machine model
| Item | Details |
|---|---|
| ISA | RV32I (37 user instructions) + `ecall`, `ebreak`, `mret`, `fence`, `fence.i`, Zicsr; `custom-0` (0x0B): only `psp.id` (funct3 = funct7 = 0, returns 'PSP1') is documented, every other encoding must trap |
| Privilege | M and U. Reset in M-mode; `mret` returns to the mode saved in `mstatus.MPP` |
| Traps | illegal instruction (2), breakpoint (3), misaligned/access faults, ecall from U (8) / M (11), shadow-stack violation (18). Entry: `mepc`, `mcause`, `mtval`, `MPP` saved, jump to `mtvec`, switch to M |
| CSRs | `mstatus`, `mtvec`, `mepc`, `mcause`, `mtval`, `mscratch`, `cycle/instret` counters; platform CSRs below |
| Memory | flat RAM at 0x8000_0000 (64 MiB default), readable/writable/executable; address 0 unmapped |
| Loading | ELF or flat binary. Programs are position-independent; `crt0` relocates the image, so code, data, bss and the 64 KiB stack move together under `--aslr` |

Platform CSRs (see `sdk/psp.h`):

| CSR | Access | Meaning |
|---|---|---|
| 0x800 | U RW | serial out (write a byte) |
| 0x801 | U RW | serial in (next byte, −1 at end of input) |
| 0x802 | U RW | exit (write = stop the simulator with that status) |
| 0xCC0 | U RO | load base of the image (a debug aid) |
| 0xFC0–0xFC7 | M RO | 32-byte "flag" string set with `--flag` (proves M-mode access in exercises) |

## Seeded defects (lab6)
A build with `DEFECTS=1` (default) contains three defects whose parameters are derived from `SEED`: an arithmetic
defect, an undocumented `custom-0` encoding that does not trap, and a decode defect in one instruction. `--golden`
disables all of them at run time; `DEFECTS=0` removes them at compile time. `src/defects.c` implements them —
don't read it before doing lab6.

## Bare-metal SDK (`sdk/`)
- `crt0.S` (reset, stack, self-relocation, calls `main`, exits with its return value), `link.ld`, `psp.h` (CSR
  macros, serial I/O, `printf` with `%x %d %u %c %s`, `psp_exit`, `psp_set_trap_handler`, `psp_enter_user`,
  `psp_hw_add` which forces a real `add` instruction), `libpsp.c`.
- `sdk.mk` provides `$(call psp_program,<out-prefix>,<sources>[,<extra cflags>])`, building
  `<out>.elf/.bin/.dis` with `clang --target=riscv32 -march=rv32i -mabi=ilp32 -nostdlib`, `ld.lld` and
  `llvm-objcopy` (versioned tool names are detected automatically). See `Makefile` (examples) or lab6's Makefile.
- Examples: `hello`, `trap_demo` (M→U, ecall, illegal instruction), `isa_test` (149-check instruction self-test),
  `aslr_demo` (relocation under `--aslr`), `ss_demo` (shadow-stack detection test), `bench`.

## Optional exercise: a hardware shadow stack (relates to L11)
With `--shadow-stack` the core classifies every `jal`/`jalr` as call, return or plain jump using the standard RISC-V
register hints (`ra`/`t0` as link registers) and calls `ss_on_call()` / `ss_on_return()` in `src/shadow_stack.c`
(interface in `src/shadow_stack.h`). As shipped the hooks allow everything. Implement them so the hardware keeps its
own copy of return addresses and refuses a mismatching return (return non-zero → trap with mcause 18). Build with
`make SHADOW_STACK_SRC=<your file>`. `examples/ss_demo` corrupts its own saved return address: without a working
shadow stack the program only crashes later at a garbage address; with one it traps exactly at the bad `ret`.
`make smoke` checks both cases plus "no false alarms" on the other examples. Reference: `solutions/shadow_stack.c`.
Questions to think about: what about `setjmp/longjmp`, coroutines and tail calls? Compare with Intel CET shadow
stacks and Arm PAC.
