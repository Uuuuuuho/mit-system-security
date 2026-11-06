# lab2-cache - cache timing, Flush+Reload, Prime+Probe, key leak, covert channel
#
#   make                  build your code       -> bin/part1 .. bin/part5
#   make check            build + grade your code (sim backend)
#   make check-solution   build + grade the reference solutions (spoilers live in solutions/)
#   make solution         only build the reference binaries -> build/solution/bin/
#   make clean
#
# Pick the backend at run time:  SHD_BACKEND=sim (default) | native   e.g.  SHD_BACKEND=native ./bin/part2
# Grade one part:                make check PARTS=2        Reproduce a run:  make check SEED=1234

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
COMMON  ?= ../common
PYTHON  ?= python3
CPPFLAGS += -Iinclude -Iplatform -I$(COMMON)
ALLCFLAGS = -std=gnu11 -pthread -MMD -MP $(CFLAGS)
LDLIBS  += -pthread -lm

PARTS_ALL := part1 part2 part3 part4 part5

PLAT_SRC := platform/plat.c platform/sim.c platform/sim_cache.c platform/native.c
PLAT_OBJ := $(PLAT_SRC:%.c=build/plat/%.o)

# student sources (the files you edit) and which ones each part's binary links
P1 := part1/calibrate.c
P2 := part2/flush_reload.c
P3 := part3/evset.c part3/prime_probe.c
P4 := part4/key_leak.c part4/victim_lookup.c
P5 := part5/sender.c part5/receiver.c
SRCS_part1 := $(P1)
SRCS_part2 := $(P1) $(P2)
SRCS_part3 := $(P1) $(P3)
SRCS_part4 := $(P1) $(P3) $(P4)
SRCS_part5 := $(P1) $(P3) $(P5)

STUDENT_BINS  := $(PARTS_ALL:%=bin/%)
SOLUTION_BINS := $(PARTS_ALL:%=build/solution/bin/%)

all: $(STUDENT_BINS)
solution: $(SOLUTION_BINS)

define PART_RULES
bin/$(1): build/drivers/$(1)_main.o $$(SRCS_$(1):%.c=build/student/%.o) $$(PLAT_OBJ)
	@mkdir -p $$(@D)
	$$(CC) $$(ALLCFLAGS) -o $$@ $$^ $$(LDLIBS)
build/solution/bin/$(1): build/drivers/$(1)_main.o $$(SRCS_$(1):%.c=build/solution/%.o) $$(PLAT_OBJ)
	@mkdir -p $$(@D)
	$$(CC) $$(ALLCFLAGS) -o $$@ $$^ $$(LDLIBS)
endef
$(foreach p,$(PARTS_ALL),$(eval $(call PART_RULES,$(p))))

build/plat/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALLCFLAGS) -c $< -o $@
build/drivers/%.o: drivers/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALLCFLAGS) -c $< -o $@
build/student/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -I$(dir $<) $(ALLCFLAGS) -c $< -o $@
build/solution/%.o: solutions/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -I$(dir $<) $(ALLCFLAGS) -c $< -o $@

check: $(STUDENT_BINS)
	$(PYTHON) check.py $(if $(PARTS),--parts $(PARTS)) $(if $(SEED),--seed $(SEED))

check-solution: $(SOLUTION_BINS)
	$(PYTHON) check.py --solution $(if $(PARTS),--parts $(PARTS)) $(if $(SEED),--seed $(SEED))

clean:
	rm -rf build bin out

-include $(shell find build -name '*.d' 2>/dev/null)

.PHONY: all solution check check-solution clean
