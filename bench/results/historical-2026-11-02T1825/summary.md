# Benchmark summary

Median [min, max] over the successful repetitions of each configuration (n=3 unless shown). Latencies are per DB call, from a monotonic clock, in microseconds. Warm page cache; request counts are engine requests, not device I/O.

Failed runs (excluded): 0

## Inserts: sync on versus the sync bypass (groups 1 and 2)

| config | ops/s | p50 us | p99 us | max us | write amp | flushes | compactions | skipped fsyncs |
|---|---|---|---|---|---|---|---|---|
| sync_on | 184 [176, 213] | 4718.7 [4596.3, 4773.5] | 7745.3 [5314.1, 7824.4] | 91999.0 [69552.7, 98253.8] | 8.715 [8.715, 8.715] | 44 [44, 44] | 14 [14, 14] | {} |
| sync_off | 78862 [78647, 79432] | 7.6 [7.6, 7.7] | 18.2 [16.6, 18.5] | 8801.0 [8784.5, 8805.2] | 8.715 [8.715, 8.715] | 44 [44, 44] | 14 [14, 14] | {"directory": 177, "manifest": 59, "sst": 58, "wal": 20045} |

Throughput ratio (median off / median on): 429.6x. Skipping sync is unsafe and only a reference point; the difference is not an exact fsync time share.

## Lookups: Bloom queries used versus bypassed (groups 1 and 3)

Same prepared files, same lookup traces. Per-lookup counters are per Get.

| config | kind | ops/s | p50 us | p99 us | candidates | filter rejections | range rejections | data blocks | bytes requested |
|---|---|---|---|---|---|---|---|---|---|
| filters used | existing | 82508 [82236, 83961] | 11.84 [11.67, 11.88] | 20.57 [19.65, 21.82] | 0.993 [0.993, 0.993] | 0.000 [0.000, 0.000] | 0.000 [0.000, 0.000] | 0.993 [0.993, 0.993] | 3968 [3968, 3968] |
| filters used | missing | 3816770 [3803090, 3896490] | 0.14 [0.14, 0.14] | 0.43 [0.34, 0.48] | 1.000 [1.000, 1.000] | 0.992 [0.992, 0.992] | 0.000 [0.000, 0.000] | 0.008 [0.008, 0.008] | 31 [31, 31] |
| bypassed | existing | 83864 [83609, 84222] | 11.68 [11.63, 11.71] | 19.50 [19.26, 19.74] | 0.993 [0.993, 0.993] | 0.000 [0.000, 0.000] | 0.000 [0.000, 0.000] | 0.993 [0.993, 0.993] | 3968 [3968, 3968] |
| bypassed | missing | 84156 [82796, 84179] | 11.65 [11.64, 11.83] | 19.24 [19.09, 19.74] | 1.000 [1.000, 1.000] | 0.000 [0.000, 0.000] | 0.000 [0.000, 0.000] | 1.000 [1.000, 1.000] | 3998 [3998, 3998] |

Prepared database: 200000 keys, 1 live runs, 1 live SSTs, 27129800 bytes.
Missing-key data-block requests per lookup: 1.000 bypassed, 0.008 with filters (99.2% fewer).
Open phase (filters used runs): 69.1 [68.4, 69.3] ms, 26908628 [26908628, 26908628] bytes requested by pread.

## Compaction run thresholds (group 4)

Same trace (overwrites, 10% deletes), same fresh initial state, same final operation; no forced maintenance. Database size counts every regular file in the directory (including any leftover orphan or temporary file) after the handle is closed.

| trigger | ops/s | p50 us | p99 us | max us | write amp | metadata / user byte | DB bytes | live runs | live SSTs | flushes | compactions | SST candidates per Get |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 171 [139, 211] | 4838.8 [4572.6, 6995.9] | 8026.8 [6223.8, 9440.9] | 101930.5 [95307.9, 107925.5] | 16.060 [16.060, 16.060] | 0.0044 [0.0044, 0.0044] | 1062351 [1062351, 1062351] | 1 [1, 1] | 1 [1, 1] | 41 [41, 41] | 40 [40, 40] | 0.998 [0.998, 0.998] |
| 4 | 187 [139, 215] | 4618.9 [4557.1, 7027.5] | 7997.7 [5411.9, 9401.4] | 95041.4 [63261.8, 98273.3] | 6.952 [6.952, 6.952] | 0.0038 [0.0038, 0.0038] | 1113291 [1113291, 1113291] | 2 [2, 2] | 2 [2, 2] | 41 [41, 41] | 13 [13, 13] | 1.948 [1.948, 1.948] |
| 8 | 157 [140, 213] | 6835.1 [4597.7, 7008.6] | 8449.9 [5333.6, 9653.7] | 90254.8 [64421.1, 104095.6] | 4.199 [4.199, 4.199] | 0.0046 [0.0046, 0.0046] | 1316872 [1316872, 1316872] | 6 [6, 6] | 6 [6, 6] | 41 [41, 41] | 5 [5, 5] | 5.321 [5.321, 5.321] |
| disabled | 174 [140, 215] | 4794.8 [4568.0, 7023.0] | 7966.1 [5113.2, 9561.3] | 54065.4 [35733.1, 54812.2] | 2.418 [2.418, 2.418] | 0.0144 [0.0144, 0.0144] | 2454265 [2454265, 2454265] | 41 [41, 41] | 41 [41, 41] | 41 [41, 41] | 0 [0, 0] | 18.221 [18.221, 18.221] |

