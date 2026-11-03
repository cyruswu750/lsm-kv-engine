# Superseded results (2026-11-02, 18:25-18:54)

These are the first complete M4 benchmark results, kept unchanged as
historical evidence. They are superseded by the results in the parent
directory.

Why: in this run, `*_ops_per_s` was calls divided by the wall time of the
whole timed loop, which also contained the harness's work between calls
(building each value, updating the reference model). For the cheap
operations this understated throughput: with the sync bypass, p50 was
about 7.6 us per call while the loop spent about 12.7 us per operation.
The fixed driver reports throughput from the summed per-call times
(`ops_per_s`) and the loop's wall time separately (`wall_ops_per_s`).

Unaffected: latency percentiles and maxima, request and byte counters,
write amplification, database sizes, SST candidates per lookup, skipped
sync counts, and the verification results. The derived figures
(about 430x for the sync bypass, -99.2% data-block requests with filters)
were recomputed from `raw/` and match `summary.md`; the 430x ratio uses
the old throughput definition.

Source: frozen snapshot with tree identity `12f22890...` (see
`environment.txt`).
