# mdbus benchmark report

Campaign: Apple M1 (MacBookAir10,1: 4 P-cores in one cluster + 4 E-cores, 8 GB), macOS 15.6, Apple clang 17.0.0, -O2, commit a53b164, 2026-11-16. Three passes of compare.py over the same runs directory (17:07 to 18:17 IST): the second and third re-ran only runs the gates had rejected; see campaign.log.

Runs: 216 (215 valid). Comparisons: 18.

## Comparisons (B - A over counterbalanced pairs)

| id | A -> B | metric | A median | B - A median | range | agree | left out | predicted | verdict |
|---|---|---|---|---|---|---|---|---|---|
| AA-e2e-p99 | Base -> Base | e2e_p99 | 9 | +0 | [0, 1.02e+03] | 1/6 | 0 | [-1, 1] hit | unresolved |
| AA-feed | F4 -> F4 | ns_per_event | 56.38 | -0.122 | [-0.397, 0.214] | 4/6 | 0 | [-1.13, 1.13] hit | no difference |
| AA-hop | Base -> Base | hop_mean_body_ns | 57.59 | -1.71 | [-3.8, 0.404] | 5/6 | 0 | [-1, 1] miss | unresolved |
| AA-wsat | Base -> Base | wsat_ns | 24.46 | +0.0775 | [-0.605, 1.61] | 3/6 | 0 | [-1, 1] hit | unresolved |
| Copy | Base -> Copy15 | hop_mean_body_ns | 55.14 | +49.3 | [42.7, 51.2] | 6/6 | 0 | [31, 38] miss | B worse |
| Dispatch | dispatch -> dispatch | dispatch_instr | 21.88 | +9.9 | [9.9, 9.9] | 6/6 | 0 | [5, 40] hit | B worse |
| F0-F1 | F0 -> F1 | ns_per_event | 165.2 | -34.4 | [-41.2, -24.8] | 6/6 | 0 | [-30, -5] miss | B better |
| F1-F2 | F1 -> F2 | ns_per_event | 124.5 | -60.6 | [-65.4, -56.9] | 6/6 | 0 | [-30, -5] miss | B better |
| F2-F3 | F2 -> F3 | ns_per_event | 65.93 | -6.43 | [-11.6, -4.73] | 6/6 | 0 | [-10, -3] hit | B better |
| F3-F4 | F3 -> F4 | ns_per_event | 58.79 | -0.931 | [-4.73, 6.38] | 4/6 | 0 | [-1.18, 1.18] hit | unresolved |
| F3-F4-32MB | F3 -> F4 | ns_per_event | 108.6 | -36.2 | [-40.6, -31.6] | 6/6 | 0 | [-32.6, 0] miss | B better |
| HeadPoll | Base -> HeadPoll | hop_mean_body_ns | 65.97 | +41.9 | [30.1, 52.5] | 5/5 | 1 | [25, 50] hit | unresolved |
| Iso-slowE | Base -> Base | hop_mean_body_ns | 66.24 | +153 | [140, 164] | 6/6 | 0 | [-2, 2] miss | B worse |
| Mutex-Base-e2e | Mutex -> Base | e2e_p99 | 2964 | -2.96e+03 | [-3.57e+03, -2.58e+03] | 6/6 | 0 | [-51, -34] miss | B better |
| Mutex-Base-wsat | Mutex -> Base | wsat_ns | 29.25 | -3.67 | [-5.49, -2.8] | 5/5 | 1 | [-58, -42] miss | unresolved |
| Pad-pp | Base -> Pad64 | hop_mean_body_ns | 54.25 | +7.15 | [3.38, 19.4] | 6/6 | 0 | [-2, 2] miss | B worse |
| Pad-wsat | Base -> Pad64 | wsat_ns | 24.21 | +6.1 | [0.102, 8.98] | 6/6 | 0 | [-2, 2] miss | B worse |
| Writer-alone | Base -> Base | wsat_ns | 2.308 | +21.8 | [21.5, 23.3] | 6/6 | 0 | [15, 25] hit | B worse |

## Latency per variant (medians over valid runs)

| variant | slow | runs | metric | body mean | mean | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| Base | off | 36 | hop | 56.4 ns | 107.3 ns | 1 t, in (0, 83) ns | 125 ns | 9083 ns | 255.8 us |
| Base | off | 36 | e2e | 56.2 ns | 170.8 ns | 1 t, in (0, 83) ns | 125 ns | 20.3 us | 328.5 us |
| Base | read | 24 | hop | 207.3 ns | 227.6 ns | 167 ns | 375 ns | 1083 ns | 136.8 us |
| Base | read | 24 | e2e | 207.4 ns | 246.7 ns | 167 ns | 375 ns | 6188 ns | 286.6 us |
| Copy15 | off | 6 | hop | 104.5 ns | 114.2 ns | 2 t, in (42, 125) ns | 167 ns | 1625 ns | 46.0 us |
| Copy15 | off | 6 | e2e | 104.6 ns | 119.2 ns | 2 t, in (42, 125) ns | 167 ns | 3604 ns | 58.8 us |
| HeadPoll | off | 6 | hop | 104.9 ns | 152.4 ns | 2 t, in (42, 125) ns | 146 ns | 12.4 us | 226.3 us |
| HeadPoll | off | 6 | e2e | 105.0 ns | 211.4 ns | 2 t, in (42, 125) ns | 333 ns | 24.2 us | 267.0 us |
| Mutex | read | 6 | hop | 281.7 ns | 12653.6 ns | 1771 ns | 80.1 us | 1015.1 us | 3678.6 us |
| Mutex | read | 6 | e2e | 295.7 ns | 26102.0 ns | 10.6 us | 123.5 us | 2275.5 us | 3709.0 us |
| Pad64 | off | 6 | hop | 61.2 ns | 200.7 ns | 1 t, in (0, 83) ns | 208 ns | 23.0 us | 362.0 us |
| Pad64 | off | 6 | e2e | 60.8 ns | 281.3 ns | 1 t, in (0, 83) ns | 1729 ns | 44.6 us | 362.4 us |

| variant | fast | runs | wsat_ns | wsat_instr | lost_fast |
|---|---|---|---|---|---|
| Base | 0 | 6 | 2.308 | 36.65 | 0 |
| Base | 1 | 30 | 24.44 | 36.65 | 2.18e+05 |
| Mutex | 1 | 5 | 29.25 | 188.5 | 6.085e+07 |
| Pad64 | 1 | 6 | 30.05 | 36.65 | 8.699e+04 |

| dispatch | runs | dispatch_ns | dispatch_instr |
|---|---|---|---|
| static | 6 | 0.9117 | 21.88 |
| virtual | 6 | 1.478 | 31.78 |

| variant | runs | ns_per_event | instr_per_event | cycles_per_event |
|---|---|---|---|---|
| F0 | 6 | 165.2 | 1204 | 482.3 |
| F1 | 12 | 126.9 | 1057 | 378.5 |
| F2 | 12 | 64.12 | 533.2 | 197 |
| F3 | 18 | 61.65 | 480.4 | 177.7 |
| F4 | 24 | 56.82 | 491.2 | 175.7 |
