# lab3-spectre-sim -- everything runs inside the specsim Python CPU model.
#
#   make                  syntax-check all Python files + simulator selftest
#   make check            grade your code in src/          (SEED=n to repeat a run)
#   make check-solution   grade the reference solution in solutions/
#   make run FILE=x.s     run one assembly file on a fresh machine (ARGS="--trace --set a0=5")
#   make clean
#
# Part 4 grades your defenses against YOUR Part 2 attack; to use the reference
# attack instead:  make check P4_ATTACK=solutions

PYTHON    ?= python3
SEED      ?=
P4_ATTACK ?=
PARTS     ?= 1,2,3,4
FILE      ?= examples/transient.s
ARGS      ?=

export SEED P4_ATTACK
export PYTHONDONTWRITEBYTECODE = 1

PYFILES := $(wildcard specsim/*.py src/*.py solutions/*.py) check.py

.PHONY: all build check check-solution run clean

all: build

build:
	@$(PYTHON) -c "import ast, sys; [ast.parse(open(f).read(), f) for f in sys.argv[1:]]; print('syntax ok:', len(sys.argv) - 1, 'files')" $(PYFILES)
	@$(PYTHON) -m specsim selftest

check:
	$(PYTHON) check.py --impl src --parts $(PARTS)

check-solution:
	$(PYTHON) check.py --impl solutions --parts $(PARTS)

run:
	$(PYTHON) -m specsim run $(FILE) $(ARGS)

clean:
	rm -rf __pycache__ specsim/__pycache__ src/__pycache__ solutions/__pycache__ out
