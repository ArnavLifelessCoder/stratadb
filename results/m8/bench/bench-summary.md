Median of 5 runs; min-max in brackets. 10,000,000 keys for fills and YCSB, 1,000,000 point reads, 1,000,000 ops per YCSB workload, 2,000,000 keys for the bloom test.

| Workload | lsm ops/s | lsm p50 us | lsm p99 us | leveldb ops/s | leveldb p50 us | leveldb p99 us |
|---|---|---|---|---|---|---|
| fillseq | 47K [44K-58K] | 11.0 | 27.4 | 105K [101K-118K] | 7.3 | 18.3 |
| fillrandom | 20K [6K-22K] | 14.3 | 41.9 | 42K [41K-47K] | 8.8 | 23.2 |
| readrandom | 99K [88K-107K] | 8.9 | 20.2 | 68K [66K-92K] | 13.2 | 29.0 |
| readseq | 1,170K [1,050K-1,346K] | - | - | 3,110K [2,638K-3,596K] | - | - |
| ycsb-a | 67K [52K-78K] | 11.9 | 29.0 | 73K [68K-84K] | 9.6 | 33.5 |
| ycsb-b | 114K [93K-123K] | 8.1 | 24.8 | 99K [92K-125K] | 9.6 | 25.1 |
| ycsb-c | 113K [99K-127K] | 8.9 | 23.3 | 109K [97K-120K] | 8.0 | 20.3 |
| ycsb-d | 187K [137K-204K] | 1.7 | 21.2 | 161K [148K-216K] | 1.7 | 21.6 |
| ycsb-e | 13K [11K-15K] | 68.0 | 260.5 | 26K [26K-29K] | 35.4 | 76.9 |
| ycsb-f | 36K [34K-44K] | 15.8 | 46.0 | 57K [53K-70K] | 14.6 | 41.3 |
| bloom10-missing | 700K [655K-774K] | 1.0 | 6.8 | 221K [160K-290K] | 2.8 | 17.1 |
| bloom0-missing | 101K [48K-109K] | 9.3 | 18.1 | 117K [95K-132K] | 5.6 | 36.6 |

| Load | Engine | WA (OS bytes, end of load) | WA (OS bytes, through close) | WA (engine stats) |
|---|---|---|---|---|
| fillseq | lsm | 4.90 | 5.03 | 4.90 |
| fillseq | leveldb | 2.19 | 2.19 | n/a |
| fillrandom | lsm | 10.36 | 11.06 | 10.36 |
| fillrandom | leveldb | 9.50 | 9.50 | n/a |

| Missing-key reads | Engine | ops/s | data blocks read per lookup |
|---|---|---|---|
| bloom 10 bits/key | lsm | 700K | 0.014 |
| bloom 10 bits/key | leveldb | 221K | not exposed |
| bloom 0 bits/key | lsm | 101K | 1.922 |
| bloom 0 bits/key | leveldb | 117K | not exposed |
