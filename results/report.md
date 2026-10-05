# mdbus benchmark report

Runs: 360 (359 valid). Comparisons: 30.

## Comparisons (B - A over counterbalanced pairs)

| id | A -> B | metric | A median | B - A median | range | agree | sign p | left out | predicted | verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| AA-e2e-p99 | Base -> Base | e2e_p99 | 9 | +0 | [0, 0] | 0/6 | 1.000 | 0 | [-1, 1] hit | no difference |
| AA-feed | F5 -> F5 | ns_per_event | 33.01 | -3.55e-15 | [-0.0916, 0.336] | 3/6 | 1.000 | 0 | [-0.66, 0.66] hit | no difference |
| AA-hop | Base -> Base | hop_mean_body_ns | 75.66 | -0.0676 | [-0.488, 0.259] | 4/6 | 0.688 | 0 | [-1, 1] hit | no difference |
| AA-wsat | Base -> Base | wsat_ns | 27.38 | +0.0713 | [-0.104, 0.14] | 5/6 | 0.219 | 0 | [-1, 1] hit | no difference |
| Copy | Base -> Copy15 | hop_mean_body_ns | 75.74 | +31.5 | [28.8, 32.7] | 6/6 | 0.031 | 0 | [31, 38] hit | B worse |
| Dispatch | dispatch -> dispatch | dispatch_instr | 21.88 | +9.9 | [9.9, 9.9] | 6/6 | 0.031 | 0 | [5, 40] hit | B worse |
| F0-F1 | F0 -> F1 | ns_per_event | 162.2 | -50.5 | [-52.8, -49.2] | 6/6 | 0.031 | 0 | [-17, -12] miss | B better |
| F1-F2 | F1 -> F2 | ns_per_event | 111.6 | -60.6 | [-61.5, -60] | 6/6 | 0.031 | 0 | [-30, -5] miss | B better |
| F2-F3 | F2 -> F3 | ns_per_event | 50.96 | -10.7 | [-12.6, -10.3] | 6/6 | 0.031 | 0 | [-30, -5] hit | B better |
| F3-F4 | F3 -> F4 | ns_per_event | 40.38 | -5.57 | [-7.75, -4.33] | 6/6 | 0.031 | 0 | [-10, -3] hit | B better |
| F4-F5 | F4 -> F5 | ns_per_event | 34.97 | -2.08 | [-3.39, -0.611] | 6/6 | 0.031 | 0 | [-0.699, 0.699] miss | B better |
| F4-F5-32MB | F4 -> F5 | ns_per_event | 53.02 | -21.4 | [-23.1, -19.3] | 6/6 | 0.031 | 0 | [-15.9, 0] miss | B better |
| HeadPoll | Base -> HeadPoll | hop_mean_body_ns | 75.92 | +32.6 | [32.1, 33.1] | 6/6 | 0.031 | 0 | [25, 50] hit | B worse |
| Iso-burnE | Base -> Base | hop_mean_body_ns | 75.9 | -0.0975 | [-0.306, 0.297] | 4/6 | 0.688 | 0 | [-2, 2] hit | no difference |
| Iso-slowE | Base -> Base | hop_mean_body_ns | 75.87 | +120 | [79, 123] | 6/6 | 0.031 | 0 | [-2, 2] miss | B worse |
| Load | Base -> LdSeqCst | hop_mean_body_ns | 75.83 | +0.176 | [-0.271, 0.597] | 4/6 | 0.688 | 0 | [-1, 1] hit | no difference |
| Mutex-Base-e2e | Mutex -> Base | e2e_p99 | 3428 | -3.42e+03 | [-4.18e+03, -3.24e+03] | 6/6 | 0.031 | 0 | [-51, -34] miss | B better |
| Mutex-Base-wsat | Mutex -> Base | wsat_ns | 25.88 | +1.6 | [-0.14, 2.61] | 5/6 | 0.219 | 0 | [-58, -42] miss | unresolved |
| Pad-pe | Base -> Pad64 | hop_mean_body_ns | 286 | +2.94 | [-3.78, 8.16] | 5/6 | 0.219 | 0 | [0, 60] hit | unresolved |
| Pad-pp | Base -> Pad64 | hop_mean_body_ns | 75.83 | -0.149 | [-0.523, 0.0166] | 3/6 | 1.000 | 0 | [-2, 2] hit | no difference |
| Pad-wsat | Base -> Pad64 | wsat_ns | 27.36 | -3.02 | [-3.44, -1.78] | 6/6 | 0.031 | 0 | [-2, 2] miss | B better |
| Pf8 | Base -> Pf8 | hop_mean_body_ns | 75.8 | +0.0141 | [-0.241, 0.121] | 3/5 | 1.000 | 1 | [-2, 2] hit | no difference |
| Pf8-wsat | Base -> Pf8 | wsat_ns | 27.38 | +0.132 | [0.0255, 0.305] | 6/6 | 0.031 | 0 | [-1, 1] hit | no difference |
| Relay-only | Base -> Base | hop_mean_body_ns | 76.05 | -9.74 | [-10.1, -9.53] | 6/6 | 0.031 | 0 | [0, 15] miss | B better |
| Relay-vs-absent | Base -> Base | hop_mean_body_ns | 76.08 | -10.1 | [-11.6, -8.61] | 6/6 | 0.031 | 0 | [0, 15] miss | B better |
| Relay-vs-hot | Base -> Base | hop_mean_body_ns | 188.9 | -124 | [-133, -97.5] | 6/6 | 0.031 | 0 | [-125, -100] hit | B better |
| Snap | Base -> SnapDouble | snap_mean_ns | 79.16 | +17.7 | [17.1, 19.5] | 6/6 | 0.031 | 0 | [-10, 10] miss | B worse |
| Wait-isb | Base -> WaitIsb | hop_mean_body_ns | 75.97 | +0.996 | [0.466, 1.15] | 6/6 | 0.031 | 0 | [2, 7] miss | unresolved |
| Wait-wfe | Base -> WaitWfe | hop_mean_body_ns | 76.09 | -2.04 | [-2.19, -1.87] | 6/6 | 0.031 | 0 | [1, 3] miss | B better |
| Writer-alone | Base -> Base | wsat_ns | 3.407 | +24 | [23.9, 24.1] | 6/6 | 0.031 | 0 | [15, 25] hit | B worse |

## Latency per variant (medians over valid runs)

| variant | slow | relay | runs | metric | body mean | mean | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|---|
| Base | burn | off | 6 | hop | 75.8 ns | 78.8 ns | 2 t, in (42, 125) ns | 125 ns | 167 ns | 57.3 us |
| Base | burn | off | 6 | e2e | 102.7 ns | 108.1 ns | 2 t, in (42, 125) ns | 167 ns | 208 ns | 66.9 us |
| Base | off | off | 78 | hop | 75.9 ns | 91.0 ns | 2 t, in (42, 125) ns | 125 ns | 4000 ns | 59.0 us |
| Base | off | off | 78 | e2e | 102.8 ns | 131.4 ns | 2 t, in (42, 125) ns | 167 ns | 9542 ns | 69.2 us |
| Base | off | on | 6 | hop | 66.2 ns | 1228.0 ns | 1 t, in (0, 83) ns | 167 ns | 480.1 us | 1513.3 us |
| Base | off | on | 6 | e2e | 95.0 ns | 3707.2 ns | 2 t, in (42, 125) ns | 792 ns | 1326.6 us | 2842.0 us |
| Base | read | off | 36 | hop | 202.2 ns | 208.1 ns | 208 ns | 375 ns | 375 ns | 55.5 us |
| Base | read | off | 36 | e2e | 227.8 ns | 238.6 ns | 208 ns | 375 ns | 750 ns | 70.1 us |
| Base | read | on | 12 | hop | 65.6 ns | 161.9 ns | 2 t, in (42, 125) ns | 146 ns | 12.9 us | 290.4 us |
| Base | read | on | 12 | e2e | 94.4 ns | 278.9 ns | 2 t, in (42, 125) ns | 208 ns | 42.5 us | 307.5 us |
| Copy15 | off | off | 6 | hop | 107.3 ns | 121.5 ns | 125 ns | 167 ns | 3458 ns | 78.9 us |
| Copy15 | off | off | 6 | e2e | 134.3 ns | 160.8 ns | 125 ns | 208 ns | 8625 ns | 80.1 us |
| HeadPoll | off | off | 6 | hop | 108.6 ns | 175.8 ns | 125 ns | 167 ns | 13.2 us | 233.7 us |
| HeadPoll | off | off | 6 | e2e | 136.1 ns | 260.6 ns | 125 ns | 458 ns | 25.9 us | 336.7 us |
| LdSeqCst | off | off | 6 | hop | 76.0 ns | 118.5 ns | 2 t, in (42, 125) ns | 167 ns | 15.1 us | 55.1 us |
| LdSeqCst | off | off | 6 | e2e | 102.9 ns | 200.7 ns | 2 t, in (42, 125) ns | 375 ns | 24.5 us | 66.4 us |
| Mutex | read | off | 6 | hop | 290.0 ns | 10137.5 ns | 2562 ns | 82.3 us | 131.4 us | 395.6 us |
| Mutex | read | off | 6 | e2e | 316.8 ns | 30008.3 ns | 18.6 us | 142.8 us | 219.1 us | 491.8 us |
| Pad64 | off | off | 6 | hop | 75.6 ns | 87.3 ns | 2 t, in (42, 125) ns | 125 ns | 1792 ns | 43.9 us |
| Pad64 | off | off | 6 | e2e | 102.5 ns | 128.7 ns | 2 t, in (42, 125) ns | 167 ns | 8604 ns | 54.2 us |
| Pf8 | off | off | 5 | hop | 75.7 ns | 148.6 ns | 2 t, in (42, 125) ns | 167 ns | 19.5 us | 156.4 us |
| Pf8 | off | off | 5 | e2e | 103.1 ns | 292.8 ns | 2 t, in (42, 125) ns | 1792 ns | 42.3 us | 391.3 us |
| SnapDouble | read | off | 6 | hop | 203.8 ns | 207.5 ns | 208 ns | 375 ns | 375 ns | 52.9 us |
| SnapDouble | read | off | 6 | e2e | 229.4 ns | 237.0 ns | 208 ns | 375 ns | 417 ns | 61.8 us |
| WaitIsb | off | off | 6 | hop | 76.9 ns | 91.2 ns | 2 t, in (42, 125) ns | 167 ns | 3000 ns | 52.3 us |
| WaitIsb | off | off | 6 | e2e | 104.2 ns | 132.8 ns | 2 t, in (42, 125) ns | 167 ns | 10.1 us | 63.2 us |
| WaitWfe | off | off | 6 | hop | 74.0 ns | 83.6 ns | 2 t, in (42, 125) ns | 125 ns | 1438 ns | 52.8 us |
| WaitWfe | off | off | 6 | e2e | 100.8 ns | 120.9 ns | 2 t, in (42, 125) ns | 167 ns | 6021 ns | 53.9 us |

| variant | fast | runs | wsat_ns | wsat_instr | lost_fast |
|---|---|---|---|---|---|
| Base | 0 | 6 | 3.407 | 49.61 | 0 |
| Base | 1 | 36 | 27.38 | 49.61 | 3.713e+04 |
| Mutex | 1 | 6 | 25.88 | 177.9 | 9.805e+07 |
| Pad64 | 1 | 6 | 24.35 | 49.61 | 9.384e+04 |
| Pf8 | 1 | 6 | 27.51 | 55.61 | 0 |

| dispatch | runs | dispatch_ns | dispatch_instr |
|---|---|---|---|
| static | 6 | 0.8837 | 21.88 |
| virtual | 6 | 1.48 | 31.78 |

| variant | runs | ns_per_event | instr_per_event | cycles_per_event |
|---|---|---|---|---|
| F0 | 6 | 162.2 | 1337 | 509.7 |
| F1 | 12 | 111.7 | 789.8 | 352.6 |
| F2 | 12 | 50.93 | 462.4 | 161.8 |
| F3 | 12 | 40.28 | 363.3 | 128.8 |
| F4 | 18 | 35.24 | 313.4 | 113 |
| F5 | 24 | 33 | 324.5 | 105.3 |
