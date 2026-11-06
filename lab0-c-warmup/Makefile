# lab0-c-warmup
#   make                  build ./tester (plain -O2) and ./tester-asan (ASan + UBSan) from src/logbook.c
#   make check            grade src/logbook.c: every check in both builds + valgrind (if installed)
#   make check-solution   grade solutions/logbook.c (builds into build/solution/)
#   make extra            build and run your own tests in tests/extra_tests.c (Challenge 0-4)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += -std=c11
SANFLAGS = -O1 -g -Wall -Wextra -std=c11 -fno-omit-frame-pointer \
           -fsanitize=address,undefined -fno-sanitize-recover=all
WRAP     = -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free
PYTHON  ?= python3

TEST_SRC = tests/tester.c tests/alloc_shim.c
DEPS     = src/logbook.h tests/alloc_shim.h $(TEST_SRC)

all: tester tester-asan

tester: src/logbook.c $(DEPS)
	$(CC) $(CFLAGS) -Isrc -Itests -o $@ src/logbook.c $(TEST_SRC) $(WRAP)

tester-asan: src/logbook.c $(DEPS)
	$(CC) $(SANFLAGS) -Isrc -Itests -o $@ src/logbook.c $(TEST_SRC) $(WRAP)

build/solution/tester: solutions/logbook.c $(DEPS) | build/solution
	$(CC) $(CFLAGS) -Isrc -Itests -o $@ solutions/logbook.c $(TEST_SRC) $(WRAP)

build/solution/tester-asan: solutions/logbook.c $(DEPS) | build/solution
	$(CC) $(SANFLAGS) -Isrc -Itests -o $@ solutions/logbook.c $(TEST_SRC) $(WRAP)

build/solution:
	mkdir -p $@

check: tester tester-asan
	$(PYTHON) check.py --dir .

check-solution: build/solution/tester build/solution/tester-asan
	$(PYTHON) check.py --dir build/solution

extra_tests: src/logbook.c src/logbook.h tests/extra_tests.c
	$(CC) $(SANFLAGS) -Isrc -o $@ src/logbook.c tests/extra_tests.c

extra: extra_tests
	./extra_tests

clean:
	rm -rf tester tester-asan extra_tests build

.PHONY: all check check-solution extra clean
