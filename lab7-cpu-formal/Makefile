# lab7-cpu-formal -- formal verification of the psp-mini core with yosys
#
#   make                  build: place the defects, assemble the programs, compile
#                         the testbench and parse the formal model (your src/ files)
#   make sim              run programs/*.s on psp-mini, co-simulating your spec
#   make bmc K=14         bounded model check of your miter (see variables below)
#   make cex              print the last counterexample as a readable trace
#   make check            grade src/            make check-solution  grade solutions/
#   make newseed          move the planted defects (new random magic values)
#   make clean
#
# Variables
#   K=14                  BMC depth in steps (step 1 = reset, 4 steps per instruction)
#   DEFECTS="glitch backdoor"   which planted defects to switch on (default: none)
#   ALU_ONLY=1            restrict the instruction stream to LUI/ADDI/ADD/XOR/SRLI
#   MUTANT=n              plant known bug n (1..14) in the implementation
#   NOSYNC=1              miter without re-anchoring (slow; for Discussion 4-2)
#   ENGINE=abc|sat        ABC bmc3 (default) or yosys' built-in `sat`
#   SOL=1                 use solutions/ instead of src/ (spoilers)
#   SEED=n                place the defects with seed n (make, bmc, sim, check)
#   PROG=programs/x.s     (make sim) run only this program;  TRACE=1 print commits

PY      ?= python3
K       ?= 14
ENGINE  ?= abc
MUTANT  ?= 0
DEFECTS ?=
SEED    ?=
PROG    ?=
WHICH   := $(if $(SOL),solutions,src)

BMC_FLAGS := --which $(WHICH) --k $(K) --engine $(ENGINE) --mutant $(MUTANT) \
             --defects "$(strip $(DEFECTS))" $(if $(ALU_ONLY),--alu-only) $(if $(NOSYNC),--nosync)
SIM_FLAGS := --which $(WHICH) --defects "$(strip $(DEFECTS))" --mutant $(MUTANT) \
             $(if $(TRACE),--trace) $(PROG)

.PHONY: all defects sim bmc cex check check-solution newseed clean help

all: defects
	@mkdir -p build/sim build/prog
	@for p in programs/*.s; do $(PY) tools/rvasm.py $$p -o build/prog/$$(basename $$p .s).hex || exit 1; done
	iverilog -g2005 -Irtl -Ibuild -o build/sim/tb_$(WHICH).vvp sim/tb.v rtl/psp_mini.v $(WHICH)/spec_core.v
	yosys -q -p "read_verilog -formal -Irtl -Ibuild rtl/psp_mini.v $(WHICH)/spec_core.v $(WHICH)/miter.v; prep -top miter"
	@echo "build ok ($(WHICH)/): next try  make sim   and   make bmc ALU_ONLY=1"

defects:
	@mkdir -p build
	@$(PY) tools/gen_defects.py build $(SEED) --keep

sim: defects
	$(PY) tools/sim.py $(SIM_FLAGS)

bmc: defects
	$(PY) tools/bmc.py $(BMC_FLAGS)

cex:
	$(PY) tools/cex_dump.py build/bmc/cex.vcd

check:
	$(PY) check.py --which src $(if $(SEED),--seed $(SEED))

check-solution:
	$(PY) check.py --which solutions $(if $(SEED),--seed $(SEED))

newseed:
	@mkdir -p build
	@$(PY) tools/gen_defects.py build $(SEED)

clean:
	rm -rf build tools/__pycache__

help:
	@sed -n '1,26p' Makefile
