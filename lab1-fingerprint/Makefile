# lab1-fingerprint
#   make                  build your code (part1/, part2/, part3/) and the victim sites into build/
#   make check            grade your code (quick mode, ~2 minutes)
#   make check-solution   grade the reference solution (builds into build/solution/)
#   make collect          full-size Part 2 dataset   -> traces/sweep.npz
#   make collect-counter  full-size Part 3 dataset   -> traces/counter.npz
#   make eval / eval-counter / plot / plot-counter   evaluate or plot those datasets
#   make clean            remove build/ and out/ (keeps traces/);  make distclean also removes traces/
#
# Knobs for collect*: LAYOUT=shared|split|none TRACES=20 DURATION=3 PERIOD=5 BUF=(collector default)
# Do not run two collections (or check targets) at the same time: they would disturb each other.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -std=gnu11 -pthread -Icommon -I../common
LDLIBS  += -lm
PYTHON  ?= python3

SITES   := news social miner video docs webmail maps search
HDRS    := common/fp.h common/collector_frame.h ../common/shd.h
SITEHDR := sites/site.h common/fp.h ../common/shd.h

LAYOUT   ?= shared
TRACES   ?= 20
DURATION ?= 3
PERIOD   ?= 5
BUF      ?=
BUFARG   := $(if $(BUF),--buf $(BUF),)

B  := build
BS := build/solution

STUDENT_BINS  := $(B)/timer $(B)/collector $(B)/collector_counter $(SITES:%=$(B)/sites/%)
SOLUTION_BINS := $(BS)/timer $(BS)/collector $(BS)/collector_counter $(SITES:%=$(BS)/sites/%)

all: $(STUDENT_BINS)
solution: $(SOLUTION_BINS)

# ---- student build ----
$(B)/timer: part1/timer.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
$(B)/collector: part2/collector.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
$(B)/collector_counter: part3/collector_counter.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
$(B)/sites/%: sites/%.c $(SITEHDR) | $(B)/sites
	$(CC) $(CFLAGS) -Isites -o $@ $< $(LDLIBS)

# ---- reference build ----
$(BS)/timer: solutions/part1/timer.c $(HDRS) | $(BS)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
$(BS)/collector: solutions/part2/collector.c $(HDRS) | $(BS)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
$(BS)/collector_counter: solutions/part3/collector_counter.c $(HDRS) | $(BS)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
$(BS)/sites/%: sites/%.c $(SITEHDR) | $(BS)/sites
	$(CC) $(CFLAGS) -Isites -o $@ $< $(LDLIBS)

$(B) $(B)/sites $(BS) $(BS)/sites:
	mkdir -p $@

# ---- grading ----
check: all
	$(PYTHON) check.py --bin $(B) --eval part2/eval.py

check-solution: solution
	$(PYTHON) check.py --bin $(BS) --eval solutions/part2/eval.py

# ---- full-size experiments (your code) ----
collect: all
	$(PYTHON) tools/collect.py --collector $(B)/collector --traces $(TRACES) --duration $(DURATION) \
	    --period $(PERIOD) --layout $(LAYOUT) $(BUFARG) --out traces/sweep$(if $(filter shared,$(LAYOUT)),,_$(LAYOUT)).npz
collect-counter: all
	$(PYTHON) tools/collect.py --collector $(B)/collector_counter --traces $(TRACES) --duration $(DURATION) \
	    --period $(PERIOD) --layout $(LAYOUT) --out traces/counter$(if $(filter shared,$(LAYOUT)),,_$(LAYOUT)).npz
eval:
	$(PYTHON) part2/eval.py traces/sweep$(if $(filter shared,$(LAYOUT)),,_$(LAYOUT)).npz
eval-counter:
	$(PYTHON) part2/eval.py traces/counter$(if $(filter shared,$(LAYOUT)),,_$(LAYOUT)).npz
plot:
	$(PYTHON) tools/plot.py traces/sweep$(if $(filter shared,$(LAYOUT)),,_$(LAYOUT)).npz
plot-counter:
	$(PYTHON) tools/plot.py traces/counter$(if $(filter shared,$(LAYOUT)),,_$(LAYOUT)).npz
timer: $(B)/timer
	./$(B)/timer

clean:
	rm -rf build out tools/__pycache__ part2/__pycache__ solutions/part2/__pycache__
distclean: clean
	rm -rf traces

.PHONY: all solution check check-solution collect collect-counter eval eval-counter plot plot-counter timer clean distclean
