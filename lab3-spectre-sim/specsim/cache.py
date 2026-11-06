"""Two-level data-cache model.

    L1D : 64 sets x  8 ways x 64 B = 32 KiB, LRU, ~4 cycles
    L2  : 2048 sets x 16 ways x 64 B =  2 MiB, LRU, ~14 cycles, inclusive of L1
          (Config.l2_repl = "random" switches L2 to random replacement)
    DRAM: ~150 cycles

Both levels are physically indexed with plain modulo indexing:
    L1 set = (addr >> 6) & 63        L2 set = (addr >> 6) & 2047
Because L2 is inclusive, evicting a line from L2 also removes it from L1.

The simulator updates cache *contents* in program order, but remembers when an
in-flight fill will actually deliver its data (`arrive`): a second access to a
line whose fill is still on its way waits for it (like merging into an MSHR)
instead of getting an instant hit.

Latencies carry a little random noise, plus rare large "interrupt" spikes, so
attack code has to cope with an occasional bad measurement.
"""

import random


class _Level:
    """One set-associative LRU cache level.  Each set is a Python list of line
    numbers ordered from least- (index 0) to most-recently used (index -1)."""

    def __init__(self, nsets, ways):
        assert nsets & (nsets - 1) == 0
        self.nsets, self.ways, self.mask = nsets, ways, nsets - 1
        self.sets = [[] for _ in range(nsets)]


class CacheHierarchy:
    LINE = 64

    def __init__(self, cfg, seed):
        self.cfg = cfg
        self.l1 = _Level(cfg.l1_sets, cfg.l1_ways)
        self.l2 = _Level(cfg.l2_sets, cfg.l2_ways)
        self.rng = random.Random(seed)
        self.l2_random = cfg.l2_repl == "random"     # "lru" (default) or "random"
        self.repl_rng = random.Random(seed + 1)
        self.arrive = {}        # line -> cycle at which an in-flight fill delivers data
        self.now = 0            # issue cycle of the current access (set by the core)
        self.stats = {"l1_hit": 0, "l2_hit": 0, "miss": 0, "flush": 0}

    # -- latency with noise --------------------------------------------------
    def _lat(self, level):
        cfg, r = self.cfg, self.rng.random()
        if level == 1:
            lat = cfg.lat_l1 + (1 if r < 0.25 else 0)
        elif level == 2:
            lat = cfg.lat_l2 + int(r * 4)
        else:
            lat = cfg.lat_mem - 10 + int(r * 30)
        if cfg.spike_prob and self.rng.random() < cfg.spike_prob:
            lat += self.rng.randint(80, 300)        # "timer interrupt"
        return lat

    def _wait_inflight(self, line, t, lat):
        """A hit on a line whose fill has not arrived yet waits for it (but never
        longer than a fresh miss would take)."""
        a = self.arrive.get(line)
        if a is None:
            return lat
        if a <= t:
            del self.arrive[line]
            return lat
        return max(lat, min(a - t, self.cfg.lat_mem + 20))

    # -- core operations --------------------------------------------------------
    def access(self, addr, t=None):
        """A normal (visible) access issued at cycle t: returns the latency and
        updates cache state (fill on miss, LRU update)."""
        if t is None:
            t = self.now
        line = addr >> 6
        s1 = self.l1.sets[line & self.l1.mask]
        s2 = self.l2.sets[line & self.l2.mask]
        if line in s1:
            s1.remove(line)
            s1.append(line)
            s2.remove(line)          # inclusive: the line is in L2 too
            s2.append(line)
            self.stats["l1_hit"] += 1
            return self._wait_inflight(line, t, self._lat(1))
        if line in s2:
            s2.remove(line)
            s2.append(line)
            self._fill_l1(line, s1)
            self.stats["l2_hit"] += 1
            lat = self._wait_inflight(line, t, self._lat(2))
        else:
            self._fill_l2(line, s2)
            self._fill_l1(line, s1)
            self.stats["miss"] += 1
            lat = self._lat(3)
        if t + lat > self.arrive.get(line, 0):
            self.arrive[line] = t + lat
        return lat

    def peek(self, addr, t=None):
        """Look the line up and return the latency it *would* have, without
        changing any cache state (no fill, no LRU update)."""
        if t is None:
            t = self.now
        line = addr >> 6
        if line in self.l1.sets[line & self.l1.mask]:
            return self._wait_inflight(line, t, self._lat(1))
        if line in self.l2.sets[line & self.l2.mask]:
            return self._wait_inflight(line, t, self._lat(2))
        return self._lat(3)

    def fill(self, addr):
        """Install a line in both levels immediately (used for store
        write-allocate and for fills deferred to retirement)."""
        line = addr >> 6
        s1 = self.l1.sets[line & self.l1.mask]
        s2 = self.l2.sets[line & self.l2.mask]
        if line in s2:
            s2.remove(line)
            s2.append(line)
        else:
            self._fill_l2(line, s2)
        if line in s1:
            s1.remove(line)
            s1.append(line)
        else:
            self._fill_l1(line, s1)

    def flush(self, addr):
        line = addr >> 6
        s1 = self.l1.sets[line & self.l1.mask]
        s2 = self.l2.sets[line & self.l2.mask]
        if line in s1:
            s1.remove(line)
        if line in s2:
            s2.remove(line)
        self.arrive.pop(line, None)
        self.stats["flush"] += 1

    def _fill_l1(self, line, s1):
        if len(s1) >= self.l1.ways:
            s1.pop(0)
        s1.append(line)

    def _fill_l2(self, line, s2):
        if len(s2) >= self.l2.ways:
            if self.l2_random:
                victim = s2.pop(self.repl_rng.randrange(len(s2)))
            else:
                victim = s2.pop(0)
            s1 = self.l1.sets[victim & self.l1.mask]   # back-invalidate (inclusive)
            if victim in s1:
                s1.remove(victim)
            self.arrive.pop(victim, None)
        s2.append(line)

    # -- introspection (simulator/grader only: attack code must not use these) --
    def level_of(self, addr):
        line = addr >> 6
        if line in self.l1.sets[line & self.l1.mask]:
            return 1
        if line in self.l2.sets[line & self.l2.mask]:
            return 2
        return 3
