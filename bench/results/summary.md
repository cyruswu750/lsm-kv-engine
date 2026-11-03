# Benchmark summary

Median [min, max] over the successful repetitions of each configuration (n=3 unless shown). Latencies are per DB call, from a monotonic clock, in microseconds, and include the two clock reads around the call (the measured overhead is listed below). Percentiles use the nearest rank. ops/s = calls / sum of per-call times; wall ops/s also counts the harness's untimed work between calls. Warm page cache; request counts are engine requests, not device I/O.

Failed runs (excluded): 0

## Inserts: sync on versus the sync bypass (groups 1 and 2)

| config | ops/s | wall ops/s | p50 us | p99 us | max us | write amp | flushes | compactions | skipped fsyncs |
|---|---|---|---|---|---|---|---|---|---|
| sync_on | 217 [159, 219] | 217 [159, 219] | 4476.6 [4433.3, 6738.4] | 5972.5 [5761.6, 9818.7] | 104545.4 [75991.1, 122345.5] | 8.715 [8.715, 8.715] | 44 [44, 44] | 14 [14, 14] | {} |
| sync_off | 78229 [77726, 78301] | 76968 [76486, 77055] | 7.8 [7.8, 7.9] | 16.4 [14.7, 19.4] | 9012.6 [9003.3, 9217.2] | 8.715 [8.715, 8.715] | 44 [44, 44] | 14 [14, 14] | {"directory": 177, "manifest": 59, "sst": 58, "wal": 20045} |

Throughput ratio (median off / median on): 360.3x. Skipping sync is unsafe and only a reference point; the difference is not an exact fsync time share.

## Lookups: Bloom queries used versus bypassed (groups 1 and 3)

Same prepared files, same lookup traces. Per-lookup counters are per Get.

| config | kind | ops/s | p50 us | p99 us | candidates | filter rejections | range rejections | data blocks | bytes requested |
|---|---|---|---|---|---|---|---|---|---|
| filters used | existing | 80823 [78595, 81140] | 12.13 [12.11, 12.35] | 20.82 [20.59, 23.83] | 0.993 [0.993, 0.993] | 0.000 [0.000, 0.000] | 0.000 [0.000, 0.000] | 0.993 [0.993, 0.993] | 3968 [3968, 3968] |
| filters used | missing | 4008240 [3986850, 4037190] | 0.14 [0.14, 0.14] | 0.38 [0.36, 0.39] | 1.000 [1.000, 1.000] | 0.992 [0.992, 0.992] | 0.000 [0.000, 0.000] | 0.008 [0.008, 0.008] | 31 [31, 31] |
| bypassed | existing | 81790 [81715, 81896] | 12.01 [11.97, 12.01] | 20.52 [20.34, 20.93] | 0.993 [0.993, 0.993] | 0.000 [0.000, 0.000] | 0.000 [0.000, 0.000] | 0.993 [0.993, 0.993] | 3968 [3968, 3968] |
| bypassed | missing | 81364 [80872, 81467] | 12.04 [12.03, 12.13] | 20.20 [20.09, 20.32] | 1.000 [1.000, 1.000] | 0.000 [0.000, 0.000] | 0.000 [0.000, 0.000] | 1.000 [1.000, 1.000] | 3998 [3998, 3998] |

Prepared database: 200000 keys, 1 live runs, 1 live SSTs, 27129800 bytes.
Missing-key data-block requests per lookup: 1.000 bypassed, 0.008 with filters (99.2% fewer).
Open phase (filters used runs): 69.1 [68.5, 70.1] ms, 26908628 [26908628, 26908628] bytes requested by pread.

## Compaction run thresholds (group 4)

Same trace (overwrites, 10% deletes), same fresh initial state, same final operation; no forced maintenance. Database size counts every regular file in the directory (including any leftover orphan or temporary file) after the handle is closed.

| trigger | ops/s | wall ops/s | p50 us | p99 us | max us | write amp | metadata / user byte | DB bytes | live runs | live SSTs | flushes | compactions | SST candidates per Get |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 139 [138, 214] | 139 [138, 214] | 6965.4 [4501.0, 6979.8] | 9981.8 [6002.7, 10063.3] | 102071.3 [87811.3, 102648.3] | 16.060 [16.060, 16.060] | 0.0044 [0.0044, 0.0044] | 1062351 [1062351, 1062351] | 1 [1, 1] | 1 [1, 1] | 41 [41, 41] | 40 [40, 40] | 0.998 [0.998, 0.998] |
| 4 | 139 [139, 217] | 139 [139, 217] | 6988.7 [4461.7, 7008.2] | 9837.5 [6130.7, 9845.1] | 99013.6 [95427.3, 108183.6] | 6.952 [6.952, 6.952] | 0.0038 [0.0038, 0.0038] | 1113291 [1113291, 1113291] | 2 [2, 2] | 2 [2, 2] | 41 [41, 41] | 13 [13, 13] | 1.948 [1.948, 1.948] |
| 8 | 140 [139, 164] | 140 [139, 164] | 6937.3 [6653.8, 7026.9] | 10196.8 [9594.0, 10983.4] | 101553.4 [92553.9, 187192.1] | 4.199 [4.199, 4.199] | 0.0046 [0.0046, 0.0046] | 1316872 [1316872, 1316872] | 6 [6, 6] | 6 [6, 6] | 41 [41, 41] | 5 [5, 5] | 5.321 [5.321, 5.321] |
| disabled | 140 [139, 141] | 140 [139, 141] | 6981.5 [6942.9, 7024.4] | 9987.1 [9773.8, 10516.8] | 59273.7 [56985.4, 151969.1] | 2.418 [2.418, 2.418] | 0.0144 [0.0144, 0.0144] | 2454265 [2454265, 2454265] | 41 [41, 41] | 41 [41, 41] | 41 [41, 41] | 0 [0, 0] | 18.221 [18.221, 18.221] |

Latency caveat for this table and threshold_p99.svg: Synchronous latency drifted with time during this run, not with the threshold: insert p50 was about 4.5 ms in the first two runs (20:36-20:38) and 6.6-7.0 ms in all ten runs from 20:39 on; the cause was not measured. These latency and throughput figures therefore cannot establish the causal effect of the threshold on latency. Per-run values: NOTES.md.

Clock overhead per timed call (median of 10,001 back-to-back steady_clock pairs, per run): 20.0 ns [20, 20] over 24 runs. It is included in every latency above and matters only for sub-microsecond calls.

