# lab4-rowhammer
#   make                 build the harness programs against YOUR code in src/      -> build/partN
#   make check           build + grade your code           (SEED=1234 to fix the seed)
#   make check-solution  build + grade the reference code in solutions/            -> build-sol/partN
#   make clean
# Optional: PARTS="2 3" restricts what `make check` grades; SHD_PAGEMAP=sim forces the Part 1 fallback.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -std=c11
LDLIBS  += -lm
PYTHON  ?= python3

SRCDIR  ?= src
BUILD   ?= build
OBJ     := $(BUILD)/obj

HDRS    := include/lab4.h dramsim/dramsim.h dramsim/dramsim_oracle.h support/pagemap.h harness/hcommon.h
LIBOBJS := $(OBJ)/dramsim.o $(OBJ)/pagemap.o $(OBJ)/hcommon.o
BINS    := $(BUILD)/part1 $(BUILD)/part2 $(BUILD)/part3 $(BUILD)/part4 $(BUILD)/part5 $(BUILD)/part6

all: $(BINS)

$(OBJ):
	@mkdir -p $@

$(OBJ)/dramsim.o: dramsim/dramsim.c dramsim/dramsim.h dramsim/dramsim_oracle.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/pagemap.o: support/pagemap.c support/pagemap.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/hcommon.o: harness/hcommon.c $(HDRS) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/%_main.o: harness/%_main.c $(HDRS) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
# student (or solution) code
$(OBJ)/user_%.o: $(SRCDIR)/%.c $(HDRS) | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/part1: $(OBJ)/part1_main.o $(OBJ)/user_part1_pagemap.o $(LIBOBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/part2: $(OBJ)/part2_main.o $(OBJ)/user_part2_timing.o $(LIBOBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/part3: $(OBJ)/part3_main.o $(OBJ)/user_part3_xor.o $(OBJ)/user_part2_timing.o $(LIBOBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/part4: $(OBJ)/part4_main.o $(OBJ)/user_part4_hammer.o $(LIBOBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/part5: $(OBJ)/part5_main.o $(OBJ)/user_part5_ecc.o $(LIBOBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/part6: $(OBJ)/part6_main.o $(OBJ)/user_part6_mitigate.o $(LIBOBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# keep going if one part does not compile, so the other parts are still graded
check:
	-@$(MAKE) --no-print-directory -k all
	$(PYTHON) check.py --bin $(BUILD) $(if $(SEED),--seed $(SEED)) $(if $(PARTS),--parts "$(PARTS)")

check-solution:
	$(MAKE) --no-print-directory BUILD=build-sol SRCDIR=solutions all
	$(PYTHON) check.py --bin build-sol --label solution $(if $(SEED),--seed $(SEED)) $(if $(PARTS),--parts "$(PARTS)")

clean:
	rm -rf build build-sol __pycache__

.PHONY: all check check-solution clean
.SECONDARY:
