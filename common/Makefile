CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pthread
selftest: selftest.c shd.h
	$(CC) $(CFLAGS) -o $@ selftest.c
run: selftest
	./selftest; SHD_TIMER=thread ./selftest
clean:
	rm -f selftest
.PHONY: run clean
