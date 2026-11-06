# lab6-cpu-fuzz - finding defects in a buggy RV32I CPU (psp-lite)
#
#   make                 build YOUR defective CPU (build/cpu/psp-lite) and the starter programs
#   make run1 run2 run3 run4   run one part on your CPU (see README for what each prints)
#   make check           grade your code on a CPU built with a FRESH random SEED
#   make check-solution  grade the reference solutions the same way
#   make clean
#
# Knobs: SEED=n (your CPU's seed; default random, remembered in build/.seed),

PSP_DIR := ../platform/psp-lite
BUILD := build
SRC ?= .
OUT ?= $(BUILD)/$(if $(filter .,$(SRC)),student,$(notdir $(SRC)))
PYTHON ?= python3

ifeq ($(origin SEED),undefined)
SEED := $(shell mkdir -p $(BUILD) && { [ -s $(BUILD)/.seed ] || od -An -N4 -tu4 /dev/urandom | tr -d ' \n' > $(BUILD)/.seed; } && cat $(BUILD)/.seed)
endif
CPU_DIR ?= $(BUILD)/cpu
CPU := $(CPU_DIR)/psp-lite

.DEFAULT_GOAL := all
PSP_BUILD := $(OUT)/sdk
include $(PSP_DIR)/sdk/sdk.mk

$(eval $(call psp_program,$(OUT)/part1/sp,$(SRC)/part1/sp.c))
$(eval $(call psp_program,$(OUT)/part2/addscan,$(SRC)/part2/addscan.c))
$(eval $(call psp_program,$(OUT)/part3/scan,$(SRC)/part3/scan.c $(SRC)/part3/trap.S))
PROGRAMS := $(OUT)/part1/sp $(OUT)/part2/addscan $(OUT)/part3/scan

all: cpu programs
	@echo "== CPU: $(CPU)   programs: $(OUT)/partN/*.elf (+ .dis disassembly)"

cpu:
	@$(MAKE) --no-print-directory -C $(PSP_DIR) BUILD=$(abspath $(CPU_DIR)) SEED=$(SEED) DEFECTS=1 all

programs: $(addsuffix .elf,$(PROGRAMS)) $(addsuffix .dis,$(PROGRAMS))

run1: all
	@echo "--- your CPU ---";   $(CPU) $(OUT)/part1/sp.elf
	@echo "--- golden CPU ---"; $(CPU) --golden $(OUT)/part1/sp.elf
run2: all
	$(CPU) --stats $(OUT)/part2/addscan.elf
run3: all
	$(CPU) $(OUT)/part3/scan.elf
run4: all
	$(PYTHON) $(SRC)/part4/fuzz.py --cpu $(CPU)

check:
	$(PYTHON) check.py

check-solution:
	$(PYTHON) check.py --solution

clean:
	rm -rf $(BUILD) part4/__pycache__ solutions/part4/__pycache__

.PHONY: all cpu programs run1 run2 run3 run4 check check-solution clean
