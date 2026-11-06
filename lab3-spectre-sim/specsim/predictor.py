"""Branch prediction: a pattern history table of 2-bit counters and a BTB.

* Conditional branches use a PHT of 2-bit saturating counters.  With the default
  `ghist_bits = 0` the PHT is indexed by the branch PC only (a per-branch bimodal
  predictor).  With `ghist_bits > 0` the index is PC XOR global history (gshare).
  Counters start at 1 ("weakly not-taken").  Predict taken when counter >= 2.
* Direct jumps (jal) need no prediction: their target is known at decode.
* Indirect jumps (jalr / jr / ret) are predicted by a BTB that remembers the last
  target per PC.  With no BTB entry the front-end stalls until the jump resolves.
* Only architecturally executed (committed) branches train the predictor.
"""


class BranchPredictor:
    def __init__(self, pht_bits=10, ghist_bits=0):
        self.size = 1 << pht_bits
        self.mask = self.size - 1
        self.ghist_bits = ghist_bits
        self.ghist_mask = (1 << ghist_bits) - 1
        self.pht = bytearray([1]) * self.size
        self.ghr = 0
        self.btb = {}

    def _index(self, pc):
        return ((pc >> 2) ^ self.ghr) & self.mask

    def predict(self, pc):
        return self.pht[((pc >> 2) ^ self.ghr) & self.mask] >= 2

    def update(self, pc, taken):
        i = ((pc >> 2) ^ self.ghr) & self.mask
        c = self.pht[i]
        if taken:
            if c < 3:
                self.pht[i] = c + 1
        elif c > 0:
            self.pht[i] = c - 1
        if self.ghist_bits:
            self.ghr = ((self.ghr << 1) | (1 if taken else 0)) & self.ghist_mask

    def btb_predict(self, pc):
        return self.btb.get(pc)

    def btb_update(self, pc, target):
        self.btb[pc] = target
