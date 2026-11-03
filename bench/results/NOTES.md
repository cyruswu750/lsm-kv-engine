# Notes on the current results

Measured 2026-11-02 20:30-21:03 on the pre-integration M4 snapshot named
in `environment.txt` (tree identity `263619eb...`: Git base `c6a410f`
plus the then-uncommitted M3 and M4 changes). They were not re-measured
on the integrated commits; the integrated engine and benchmark driver
differ from that snapshot only in source comments. `raw/`,
`environment.txt`, and `progress.log` are unchanged from the run.
`summary.md` and the charts were regenerated from `raw/` by the committed
`scripts/summarize_bench.py`; compared with the copies the run produced,
only the clock-overhead sentence and the threshold latency caveat
(`threshold_caveat.txt`) differ. Superseded results are in
`historical-2026-11-02T1825/` (see its `SUPERSEDED.md`).

## Observed variation in synchronous runs

Synchronous latency changed with time, not with the configuration. The
first run (20:30) had a p50 of 6.7 ms, the next five (20:33-20:38) about
4.5 ms, and every synchronous run from 20:39 on about 7.0 ms, whatever
its threshold. The
cause was not measured (the WSL2 virtual disk and the host are the likely
contributors). Consequently, the group 4 throughput and p50/p99 columns
mostly reflect when each run happened and must not be read as threshold
effects: they cannot establish the causal effect of the threshold on
latency. The threshold comparison rests on the deterministic columns
(write amplification, database size, runs, SSTs, candidates per lookup),
which are identical in all repetitions. No run was repeated or dropped.

| group | config | rep | started | ops/s | p50 ms | p99 ms |
|---|---|---|---|---|---|---|
| insert_sync | sync_on | 1 | 20:30:59 | 159 | 6.74 | 9.82 |
| insert_sync | sync_on | 2 | 20:33:06 | 217 | 4.48 | 5.97 |
| insert_sync | sync_on | 3 | 20:34:38 | 219 | 4.43 | 5.76 |
| threshold | trigger_2 | 1 | 20:36:22 | 214 | 4.50 | 6.00 |
| threshold | trigger_4 | 1 | 20:37:56 | 217 | 4.46 | 6.13 |
| threshold | trigger_8 | 1 | 20:39:28 | 164 | 6.65 | 10.20 |
| threshold | trigger_0 | 1 | 20:41:30 | 141 | 6.94 | 10.52 |
| threshold | trigger_2 | 2 | 20:43:53 | 138 | 6.98 | 9.98 |
| threshold | trigger_4 | 2 | 20:46:18 | 139 | 6.99 | 9.84 |
| threshold | trigger_8 | 2 | 20:48:42 | 140 | 6.94 | 10.98 |
| threshold | trigger_0 | 2 | 20:51:05 | 140 | 6.98 | 9.77 |
| threshold | trigger_2 | 3 | 20:53:28 | 139 | 6.97 | 10.06 |
| threshold | trigger_4 | 3 | 20:55:52 | 139 | 7.01 | 9.85 |
| threshold | trigger_8 | 3 | 20:58:17 | 139 | 7.03 | 9.59 |
| threshold | trigger_0 | 3 | 21:00:40 | 139 | 7.02 | 9.99 |
