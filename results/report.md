# mdbus benchmark report

Campaign: M1 Pro (8 P-cores in two clusters + 2 E-cores), macOS 15.6, Apple clang 15, -O2, commit 7c95cde, 2026-10-06

Measured at 7c95cde; library and bench code are identical to e7022a6 except comments.

Runs: 216 (215 valid). Comparisons: 18.

## Comparisons (B - A over counterbalanced pairs)

| id | A -> B | metric | A median | B - A median | range | agree | left out | predicted | verdict |
|---|---|---|---|---|---|---|---|---|---|
| AA-e2e-p99 | Base -> Base | e2e_p99 | 10 | +0 | [0, 3.82e+05] | 1/6 | 0 | [-1, 1] hit | unresolved |
| AA-feed | F4 -> F4 | ns_per_event | 52.87 | +0.0305 | [-0.397, 0.275] | 4/6 | 0 | [-1.06, 1.06] hit | no difference |
| AA-hop | Base -> Base | hop_mean_body_ns | 90.57 | -2.1 | [-8, 6.3] | 4/6 | 0 | [-1, 1] miss | unresolved |
| AA-wsat | Base -> Base | wsat_ns | 26.07 | +0.0699 | [-0.234, 0.186] | 4/6 | 0 | [-1, 1] hit | no difference |
| Copy | Base -> Copy15 | hop_mean_body_ns | 86.85 | +42 | [36.6, 43.5] | 6/6 | 0 | [31, 38] miss | B worse |
| Dispatch | dispatch -> dispatch | dispatch_instr | 21.9 | +9.9 | [9.9, 9.9] | 6/6 | 0 | [5, 40] hit | B worse |
| F0-F1 | F0 -> F1 | ns_per_event | 147.5 | -29.9 | [-40.2, -29.2] | 6/6 | 0 | [-30, -5] hit | B better |
| F1-F2 | F1 -> F2 | ns_per_event | 130.5 | -63.8 | [-70, -58.5] | 5/5 | 1 | [-30, -5] miss | unresolved |
| F2-F3 | F2 -> F3 | ns_per_event | 61.89 | -7.02 | [-7.6, -5.34] | 6/6 | 0 | [-10, -3] hit | B better |
| F3-F4 | F3 -> F4 | ns_per_event | 54.67 | -1.62 | [-2.47, -1.19] | 6/6 | 0 | [-1.09, 1.09] miss | unresolved |
| F3-F4-32MB | F3 -> F4 | ns_per_event | 99.95 | -37.2 | [-39, -30.2] | 6/6 | 0 | [-30, 0] miss | B better |
| HeadPoll | Base -> HeadPoll | hop_mean_body_ns | 91.32 | +29.7 | [10.2, 47.6] | 6/6 | 0 | [25, 50] hit | B worse |
| Iso-slowE | Base -> Base | hop_mean_body_ns | 86.87 | +133 | [113, 144] | 6/6 | 0 | [-2, 2] miss | B worse |
| Mutex-Base-e2e | Mutex -> Base | e2e_p99 | 5691 | -5.68e+03 | [-6.61e+03, -5.55e+03] | 6/6 | 0 | [-51, -34] miss | B better |
| Mutex-Base-wsat | Mutex -> Base | wsat_ns | 25.67 | +0.518 | [-0.758, 0.936] | 5/6 | 0 | [-58, -42] miss | no difference |
| Pad-pp | Base -> Pad64 | hop_mean_body_ns | 87.02 | +0.919 | [0.274, 4.38] | 6/6 | 0 | [-2, 2] hit | no difference |
| Pad-wsat | Base -> Pad64 | wsat_ns | 26.06 | -1.75 | [-3.37, -1.47] | 6/6 | 0 | [-2, 2] hit | B better |
| Writer-alone | Base -> Base | wsat_ns | 2.668 | +23.4 | [23.3, 23.6] | 6/6 | 0 | [15, 25] hit | B worse |

## Latency per variant (medians over valid runs)

| variant | slow | runs | metric | body mean | mean | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| Base | off | 36 | hop | 87.8 ns | 107.7 ns | 2 t, in (42, 125) ns | 250 ns | 6458 ns | 52.4 us |
| Base | off | 36 | e2e | 87.7 ns | 137.5 ns | 2 t, in (42, 125) ns | 333 ns | 10.3 us | 108.6 us |
| Base | read | 24 | hop | 222.9 ns | 229.1 ns | 208 ns | 417 ns | 458 ns | 38.1 us |
| Base | read | 24 | e2e | 223.1 ns | 233.0 ns | 208 ns | 417 ns | 1917 ns | 49.0 us |
| Copy15 | off | 6 | hop | 127.5 ns | 138.8 ns | 125 ns | 333 ns | 2979 ns | 43.8 us |
| Copy15 | off | 6 | e2e | 127.7 ns | 149.5 ns | 125 ns | 354 ns | 6604 ns | 46.2 us |
| HeadPoll | off | 6 | hop | 116.5 ns | 128.8 ns | 125 ns | 438 ns | 3396 ns | 61.6 us |
| HeadPoll | off | 6 | e2e | 116.5 ns | 143.4 ns | 125 ns | 667 ns | 7875 ns | 83.9 us |
| Mutex | read | 6 | hop | 281.0 ns | 13381.5 ns | 3938 ns | 97.3 us | 149.0 us | 766.8 us |
| Mutex | read | 6 | e2e | 279.5 ns | 44509.5 ns | 27.8 us | 237.1 us | 420.5 us | 895.6 us |
| Pad64 | off | 6 | hop | 88.6 ns | 101.3 ns | 2 t, in (42, 125) ns | 250 ns | 3250 ns | 43.3 us |
| Pad64 | off | 6 | e2e | 88.7 ns | 112.2 ns | 2 t, in (42, 125) ns | 292 ns | 6146 ns | 45.8 us |

| variant | fast | runs | wsat_ns | wsat_instr | lost_fast |
|---|---|---|---|---|---|
| Base | 0 | 6 | 2.668 | 45.68 | 0 |
| Base | 1 | 30 | 26.09 | 45.68 | 0 |
| Mutex | 1 | 6 | 25.67 | 176.3 | 9.476e+07 |
| Pad64 | 1 | 6 | 24.31 | 45.68 | 8164 |

| dispatch | runs | dispatch_ns | dispatch_instr |
|---|---|---|---|
| static | 6 | 0.8596 | 21.9 |
| virtual | 6 | 1.442 | 31.81 |

| variant | runs | ns_per_event | instr_per_event | cycles_per_event |
|---|---|---|---|---|
| F0 | 6 | 147.5 | 1214 | 476.3 |
| F1 | 11 | 119.7 | 1067 | 384.6 |
| F2 | 12 | 63.25 | 544.5 | 200.1 |
| F3 | 18 | 54.93 | 473.5 | 177.8 |
| F4 | 24 | 52.95 | 485.4 | 172.4 |
