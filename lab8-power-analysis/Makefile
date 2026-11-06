# lab8-power-analysis -- physical side channels on synthetic traces (Python/numpy simulator only)
#
#   make                  syntax-check all Python files + simulator selftest
#   make check            grade your code in src/          (SEED=n repeats a run, PARTS=2,3 grades a subset)
#   make check-solution   grade the reference solution in solutions/ (including the optional 4-2)
#   make show DEV=aes     text view of one trace (DEV = aes | modexp | masked | delays | shuffle; ARGS="--full")
#   make clean
#
# Everything is deterministic for a given SEED and runs the same on x86_64 and aarch64.

PYTHON ?= python3
SEED   ?=
PARTS  ?= 1,2,3,4
DEV    ?= aes
ARGS   ?=

export SEED
export PYTHONDONTWRITEBYTECODE = 1
export MPLBACKEND = Agg

PYFILES := $(wildcard powersim/*.py src/*.py solutions/*.py) check.py

.PHONY: all build check check-solution show clean

all: build

build:
	@$(PYTHON) -c "import ast, sys; [ast.parse(open(f).read(), f) for f in sys.argv[1:]]; print('syntax ok:', len(sys.argv) - 1, 'files')" $(PYFILES)
	@$(PYTHON) -m powersim selftest

check:
	$(PYTHON) check.py --impl src --parts $(PARTS)

check-solution:
	$(PYTHON) check.py --impl solutions --parts $(PARTS) --require-bonus

show:
	$(PYTHON) -m powersim show $(DEV) $(ARGS)

clean:
	rm -rf __pycache__ powersim/__pycache__ src/__pycache__ solutions/__pycache__ out
