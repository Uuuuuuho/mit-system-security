# SPOILERS

Reference solutions for lab8-power-analysis, with the same file names as `src/`.
Try the lab first. `make check-solution` grades these files (including the optional Exercise 4-2).

Notes for instructors/self-checkers:
* part1_spa.py: least-squares template match for the start, layout walk with a 3-sample decision;
  1-2 aligns with FFT cross-correlation and re-aligns twice against the average.
* part2_dpa.py: multi-bit difference of means (sum of the 8 single-bit peak heights).
* part3_cpa.py: rank_curve uses running sums, so its cost is one pass over the traces.
* part4_countermeasures.py: masked AES after Herbst-Oswald-Mangard (m_in, m_out, one mask per
  MixColumns row); TVLA-clean in the HW model and, by design, leaky in the HD model (Discussion 4-2).
