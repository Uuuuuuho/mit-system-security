# psp-lite: RV32I instruction-set simulator + bare-metal SDK (shd-labs shared platform).
#
#   make                      build $(BUILD)/psp-lite
#   make examples             build the SDK examples into $(BUILD)/examples/
#   make smoke                build everything and run the smoke test (tests/smoke.py)
#   make clean
#
# Build-time knobs (all optional):
#   BUILD=dir                 output directory (default: build)
#   SEED=n                    seeds the hidden defects (default: random, remembered in $(BUILD)/.seed)
#   DEFECTS=0                 compile the seeded defects out entirely (a "golden-only" CPU)
#   SHADOW_STACK_SRC=file.c   optional exercise: your shadow-stack implementation (default: src/shadow_stack.c stub)
#
# Grader-only: `make defects-dump` builds $(BUILD)/psp-defects, which prints the defect
# parameters of a given SEED.  Using it while doing lab6 spoils the lab.

BUILD ?= build
DEFECTS ?= 1
SHADOW_STACK_SRC ?= src/shadow_stack.c
CC ?= cc
HOST_CFLAGS ?= -O2 -Wall -Wextra -std=c11
PYTHON ?= python3

ifeq ($(origin SEED),undefined)
SEED := $(shell mkdir -p $(BUILD) && { [ -s $(BUILD)/.seed ] || od -An -N4 -tu4 /dev/urandom | tr -d ' \n' > $(BUILD)/.seed; } && cat $(BUILD)/.seed)
endif

SS_ABS := $(abspath $(SHADOW_STACK_SRC))
CONFIG := SEED=$(SEED) DEFECTS=$(DEFECTS) SS=$(SS_ABS) CC=$(CC) CFLAGS=$(HOST_CFLAGS)
# rewrite the config stamp only when it changes, so `make` rebuilds exactly when needed
_dummy := $(shell mkdir -p $(BUILD) && { printf '%s\n' '$(CONFIG)' | cmp -s - $(BUILD)/config.txt || printf '%s\n' '$(CONFIG)' > $(BUILD)/config.txt; })

ISS_SRCS := src/main.c src/cpu.c src/disasm.c src/loader.c src/defects.c
ISS_HDRS := src/iss.h src/shadow_stack.h
ISS_DEFS := -DPSP_SEED=$(SEED)ull -DPSP_DEFECTS=$(DEFECTS) -DPSP_SS_SRC_NAME='"$(notdir $(SHADOW_STACK_SRC))"'

all: $(BUILD)/psp-lite

$(BUILD)/psp-lite: $(ISS_SRCS) $(ISS_HDRS) $(SS_ABS) $(BUILD)/config.txt
	@mkdir -p $(@D)
	$(CC) $(HOST_CFLAGS) $(ISS_DEFS) -Isrc -o $@ $(ISS_SRCS) $(SS_ABS)

defects-dump: $(BUILD)/psp-defects
$(BUILD)/psp-defects: tools/defects_dump.c src/defects.c $(ISS_HDRS) $(BUILD)/config.txt
	@mkdir -p $(@D)
	$(CC) $(HOST_CFLAGS) $(ISS_DEFS) -DPSP_DEFECTS_DUMP -Isrc -o $@ tools/defects_dump.c src/defects.c

# ---------------------------------------------------------------- SDK examples
PSP_BUILD := $(BUILD)/examples/rt
include sdk/sdk.mk
EXAMPLES := hello aslr_demo trap_demo isa_test ss_demo bench
$(foreach e,$(filter-out ss_demo,$(EXAMPLES)),$(eval $(call psp_program,$(BUILD)/examples/$(e),examples/$(e).c)))
# the shadow-stack demo keeps a frame pointer so its saved return address is at -4(s0)
$(eval $(call psp_program,$(BUILD)/examples/ss_demo,examples/ss_demo.c,-fno-omit-frame-pointer))
examples: $(foreach e,$(EXAMPLES),$(BUILD)/examples/$(e).elf $(BUILD)/examples/$(e).bin)

# a second simulator with the reference shadow stack, used by the smoke test
$(BUILD)/ss-ref/psp-lite: $(ISS_SRCS) $(ISS_HDRS) solutions/shadow_stack.c
	@mkdir -p $(@D)
	$(CC) $(HOST_CFLAGS) -DPSP_SEED=1ull -DPSP_DEFECTS=0 -DPSP_SS_SRC_NAME='"solutions/shadow_stack.c"' -Isrc \
	    -o $@ $(ISS_SRCS) solutions/shadow_stack.c

smoke test: $(BUILD)/psp-lite $(BUILD)/ss-ref/psp-lite examples
	$(PYTHON) tests/smoke.py --build $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all examples smoke test clean defects-dump
