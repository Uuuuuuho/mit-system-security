# sdk.mk - build bare-metal programs for psp-lite.  Include it from your Makefile:
#
#     PSP_BUILD := build/psp-sdk          # where the runtime objects go (optional)
#     include path/to/platform/psp-lite/sdk/sdk.mk
#     $(eval $(call psp_program,build/hello,hello.c util.c))
#     $(eval $(call psp_program,build/vuln,vuln.c,-O0 -fno-inline))   # extra CFLAGS
#
# sdk.mk defines rules, so put your default target first (or set .DEFAULT_GOAL) before including it.
# Each psp_program call defines   <stem>.elf  <stem>.bin (flat image)  <stem>.dis (disassembly)
# Objects go to <stem>.objs/.  Toolchain: clang (riscv32 target), ld.lld, llvm-objcopy/objdump;
# override PSP_CC / PSP_LD / PSP_OBJCOPY / PSP_OBJDUMP if yours have version suffixes.

PSP_SDK := $(abspath $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST)))))
PSP_BUILD ?= build/psp-sdk

psp_find = $(firstword $(foreach p,$(1),$(shell command -v $(p) 2>/dev/null)))
ifndef PSP_CC
PSP_CC := $(or $(call psp_find,clang clang-19 clang-18 clang-17 clang-16 clang-15 clang-14),clang)
endif
ifndef PSP_LD
PSP_LD := $(or $(call psp_find,ld.lld ld.lld-19 ld.lld-18 ld.lld-17 ld.lld-16 ld.lld-15 ld.lld-14),ld.lld)
endif
ifndef PSP_OBJCOPY
PSP_OBJCOPY := $(or $(call psp_find,llvm-objcopy llvm-objcopy-19 llvm-objcopy-18 llvm-objcopy-17 llvm-objcopy-16 llvm-objcopy-15 llvm-objcopy-14),llvm-objcopy)
endif
ifndef PSP_OBJDUMP
PSP_OBJDUMP := $(or $(call psp_find,llvm-objdump llvm-objdump-19 llvm-objdump-18 llvm-objdump-17 llvm-objdump-16 llvm-objdump-15 llvm-objdump-14),llvm-objdump)
endif

PSP_ARCH := --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 -mcmodel=medany
PSP_OPT ?= -O2
PSP_CFLAGS := $(PSP_ARCH) $(PSP_OPT) -g -std=gnu11 -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
              -fno-stack-protector -fPIE -fvisibility=hidden -fno-asynchronous-unwind-tables \
              -fno-unwind-tables -I$(PSP_SDK)
PSP_ASFLAGS := $(PSP_ARCH) -fPIE -I$(PSP_SDK)
PSP_LDFLAGS := -m elf32lriscv -pie --no-dynamic-linker -z norelro --hash-style=sysv -T $(PSP_SDK)/link.ld
PSP_RT_OBJS := $(PSP_BUILD)/crt0.o $(PSP_BUILD)/libpsp.o

$(PSP_BUILD)/crt0.o: $(PSP_SDK)/crt0.S
	@mkdir -p $(@D)
	$(PSP_CC) $(PSP_ASFLAGS) -c -o $@ $<

$(PSP_BUILD)/libpsp.o: $(PSP_SDK)/libpsp.c $(PSP_SDK)/psp.h
	@mkdir -p $(@D)
	$(PSP_CC) $(PSP_CFLAGS) -c -o $@ $<

# $(1) object, $(2) source, $(3) extra flags
define psp_obj_rule
$(1): $(2) $(PSP_SDK)/psp.h
	@mkdir -p $$(@D)
	$$(PSP_CC) $(if $(filter %.S %.s,$(2)),$$(PSP_ASFLAGS),$$(PSP_CFLAGS)) $(3) -c -o $$@ $$<
endef

# $(1) output stem, $(2) sources, $(3) extra CFLAGS
define psp_program
$(1).elf: $(foreach s,$(2),$(1).objs/$(notdir $(s)).o) $(PSP_RT_OBJS) $(PSP_SDK)/link.ld
	$$(PSP_LD) $$(PSP_LDFLAGS) -o $$@ $(foreach s,$(2),$(1).objs/$(notdir $(s)).o) $(PSP_RT_OBJS)
$(1).bin: $(1).elf
	$$(PSP_OBJCOPY) -O binary $$< $$@
$(1).dis: $(1).elf
	$$(PSP_OBJDUMP) -d -M no-aliases $$< > $$@
$(foreach s,$(2),$(eval $(call psp_obj_rule,$(1).objs/$(notdir $(s)).o,$(s),$(3))))
endef
