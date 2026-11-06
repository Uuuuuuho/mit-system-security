# lab1-fingerprint report

Name:
Date:
Machine(s) (CPU, cores, LLC size from sysfs, bare metal / VM / Docker):
`make check` score line:

## Discussion 1-1: timer resolution vs number of accesses

(paste the table printed by ./build/timer)

| clock | smallest measurable N | back-of-the-envelope explanation |
|---|---|---|
| native | | |
| 0.1 ms | | |
| 1 ms | | |

## Discussion 1-2: averaging, jitter, and what it means for P and the buffer size

## Discussion 2-1: choice of P and buffer size

| P (ms) | buffer | traces/site x T | accuracy |
|---|---|---|---|
| | | | |

Trade-offs:

## Discussion 2-2: three traces per site

(include out/sweep_traces.png)

## Discussion 2-3: confusions and classifier improvements

## Discussion 3-1: sweep vs counter, shared vs split

| collector | LAYOUT=shared | LAYOUT=split |
|---|---|---|
| sweep | | |
| counter | | |

What each comparison isolates:

## Discussion 3-2: why does the counter collector still work?

Experiment you ran (or propose) to separate two mechanisms, and its result:

## Discussion 3-3: defenses

| defense | stops sweep? | stops counter? | layout | cost |
|---|---|---|---|---|
| coarser timers / jitter | | | | |
| LLC partitioning | | | | |
| no co-scheduling across trust domains | | | | |
| randomized / batched interrupts | | | | |
| victim cover traffic | | | | |

What you would deploy in a browser, and why:
