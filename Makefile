# shd-labs top level.  Each lab also has its own Makefile (make / make check / make check-solution / make clean).
LABS := lab0-c-warmup lab1-fingerprint lab2-cache lab3-spectre-sim lab4-rowhammer lab6-cpu-fuzz lab7-cpu-formal lab8-power-analysis

.PHONY: help selftest smoke check check-solution clean $(LABS)
help:
	@echo "make selftest        timing self-test of this machine (common/)"
	@echo "make smoke           build + smoke-test the psp-lite platform"
	@echo "make check           grade YOUR code in every lab (continues past failures)"
	@echo "make check-solution  grade every reference solution"
	@echo "make clean           remove all build outputs"
	@echo "Single lab: make -C lab2-cache check"

selftest:
	$(MAKE) -C common run

smoke:
	$(MAKE) -C platform/psp-lite smoke

check:
	@for d in $(LABS); do echo "=================== $$d"; $(MAKE) --no-print-directory -C $$d check || true; done

check-solution:
	@fail=; for d in $(LABS); do echo "=================== $$d"; $(MAKE) --no-print-directory -C $$d check-solution || fail="$$fail $$d"; done; \
	 if [ -n "$$fail" ]; then echo "FAILED:$$fail"; exit 1; else echo "all labs: reference solutions pass"; fi

clean:
	@for d in $(LABS) platform/psp-lite common; do $(MAKE) --no-print-directory -C $$d clean >/dev/null || true; done
