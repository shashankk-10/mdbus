# mdbus

A shared-memory market-data bus in C++20 (header-only), for Apple Silicon macOS. One writer
process publishes order-book updates and trades into a broadcast ring in shared memory; any
number of reader processes read it without locks, and the writer never waits for a reader. An
exchange simulator and an L3 feed handler sit in front, so the path from a UDP datagram to a
reader's book runs for real.

Measured on an Apple M1 Pro (8 P-cores in two clusters, 2 E-cores), macOS 15.6, Apple clang 15.
The design, every decision and every measurement: [`DESIGN.md`](DESIGN.md).

## The pipeline

```
mdbus_exchange_sim --UDP multicast--> mdbus_feed_handler --shared memory--> readers
 (exchange)          ITCH-like packets   (order book + Publisher)   (ring)     (Consumer)
```

- The simulator sends seeded order events (add, cancel, execute, replace), up to 10 per
  numbered datagram.
- The feed handler, the bus's only writer, applies each event to its order book and publishes
  what changed in the top 6 levels, each trade, and a status when an instrument goes bad.
- A bus is one shared-memory segment: a 16,384-slot ring that readers poll without writing any
  shared memory, a seqlock snapshot per instrument for recovery, and a control block.
- Readers derive from the CRTP `Consumer` class; `mdbus_watch` is the smallest complete one.

## Highlights

- Wait-free publish and poll: a per-slot seqlock whose stamp holds the sequence number, so a
  reader that falls behind detects it exactly.
- Lazy per-instrument recovery from snapshots after a reader is lapped.
- Ring hop between two P-cores: 87.8 ns mean (p50 2 ticks of the 24 MHz counter).
- Order book: 53 ns per event, from 147.5 ns for a `std::map`/`unordered_map` design.
- Memory ordering tested on real cores by mutation: every weakened fence or ordering must be
  caught.

## Where things are

Start with the six files in **bold**, in order: they are the whole bus.

| File | What it holds |
|---|---|
| **`include/mdbus/constants.hpp`** | The bus's shared constants: slot count, payload words, cache-line sizes, liveness timings |
| `include/mdbus/status.hpp` | `Status` error codes (the library never throws), `status_name`, `check_or_abort` |
| `include/mdbus/clock.hpp` | The two clocks: 24 MHz hardware ticks for latency, steady_clock ns for heartbeats |
| `include/mdbus/atomic_policy.hpp` | `StdAtomics`: the shared word type and its six load/store/fence operations |
| `include/mdbus/messages.hpp` | The three bus messages and the `Schema` typelist that checks and dispatches them |
| **`include/mdbus/ring_slot.hpp`** | One ring slot: stamp, `MessageHeader`, payload words, the payload checksum |
| `include/mdbus/control_block.hpp` | The block at offset 0 of a segment: identity + ready marker, writer liveness, head hint |
| **`include/mdbus/ring.hpp`** | **The protocol**: `RingWriter::publish` and `RingReader::try_poll` |
| **`include/mdbus/snapshot_table.hpp`** | Per-instrument recovery snapshots: one seqlock record each, `SnapshotTable` |
| `include/mdbus/segment_format.hpp` | `BusLayout` (a bus's compile-time shape); where each part sits in the segment; build it, and validate a mapped one (layout version and ring geometry) |
| `include/mdbus/shared_memory.hpp` | `SharedMemoryMapping`: one mmap of shared memory, pre-faulted and locked |
| `include/mdbus/bus_paths.hpp` | Bus name rules, the shm name and lock-file path, `destroy_bus` |
| `include/mdbus/writer_liveness.hpp` | `WriterLock` (one writer per bus: a flock) and `judge_writer` (Alive or Down, from the writer's state word and heartbeat) |
| **`include/mdbus/publisher.hpp`** | `Publisher`: encode a message, publish it, keep the instrument's snapshot current |
| `include/mdbus/bus_writer.hpp` | `BusWriter`: create and own a named bus, replacing any old segment |
| `include/mdbus/bus_reader.hpp` | `BusReader`: open and validate a named bus read-only |
| **`include/mdbus/consumer.hpp`** | The CRTP `Consumer`: poll, dispatch to `on()` handlers, lazy recovery, writer health; the wait policies `SpinWait` and `SleepWait` |
| `include/mdbus/book/` | Order events, top levels, price ladder, order table, `FeedBook`, publishing its output |
| `include/mdbus/feed/` | `wire_format.hpp` (ITCH-shaped packets, encode/decode, gap tracking), `multicast_socket.hpp` |
| `src/` | `mdbus_feed_handler` (the bus's writer), `mdbus_watch` (the shortest complete reader), `command_line.hpp` (exit codes, strict numbers, the feed programs' option loop) |
| `sim/` | Stands in for the exchange: `mdbus_exchange_sim` and its seeded order-event generator |
| `baseline/` | Never part of the system, only measured and checked against: the naive book and the ladder F0..F4 to the final one, the bus variants, the mutex ring |
| `bench/` | `bus_bench.cpp`, `feed_bench.cpp`, `dispatch_bench.cpp`; the workload, timing, measurement, command-line, launcher and result-row helpers; `decisions.yaml` (predictions); `bench_tools_test.py` (tests of the scripts and bench command lines) |
| `tests/` | Unit tests, the book oracle, end to end over UDP, failure injection with real processes, the mutant stress, two compile-fail tests; shared code in `test_harness.hpp`, `test_main.cpp` (the one `main`) and `test_helpers.hpp` |
| `scripts/` | `compare.py` (paired comparisons), `report.py` (their tables), `check.sh` (three builds: as is, ASan+UBSan, TSan), `demo.sh` |
| `results/` | The benchmark campaign: `report.md` (comparisons and per-variant tables), `runs.csv` (every run), `campaign.log` |

## Build, test, run

Needs Apple Silicon macOS, CMake 3.25+, and for `compare.py` a python3 with PyYAML. Configuring
downloads HdrHistogram once per build directory (offline: add
`-DFETCHCONTENT_SOURCE_DIR_HDR_HISTOGRAM=DIR` with its unpacked source).

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
    ctest --test-dir build -j4      # 33 tests, about 20 s
    scripts/check.sh                # the same as built, under ASan+UBSan and under TSan
    scripts/demo.sh 10              # live: simulator -> feed handler -> bus -> viewer

Benchmarks:

    ./build/bench/mdbus_bench_Base --duration-ms 3000 --out /tmp/base      # one run
    ./build/bench/mdbus_bench_Base --slow read --out /tmp/slow             # with a slow E-core reader
    python3 scripts/compare.py Copy --out /tmp/copy                       # one paired comparison
    python3 scripts/compare.py --all --out /tmp/campaign                  # all 18, about 17 min
    ./build/mutant_stress                                                 # the mutation test

Each benchmark run writes a `row.csv` and prints a summary: latency (body mean, mean over all
messages, p50, p99, p99.9), and `conditions`, the validity gates it failed if the machine was
busy. A bad command line is refused with a reason and exit code 2.

## Results

Full tables, method and comments: `DESIGN.md` §8; raw data: `results/`.

| Comparison | Result |
|---|---|
| Ring hop, P-core to P-core, 1 M msg/s | 87.8 ns body mean; p50 2 ticks, p99 250 ns |
| Payload in one 64 B line vs two | 86.9 -> 128.9 ns |
| Readers poll the slot stamp vs a global head | 91.3 vs 121.0 ns |
| Slow E-core reader present vs absent | 86.9 -> 219.9 ns (2.5x) |
| Writer per publish, alone vs one reader | 2.67 -> 26.1 ns (same 45.7 instructions) |
| Mutex vs ring, e2e p99 with a slow reader | 237 us vs 417 ns |
| Virtual handler vs CRTP | +9.9 instructions per message |
| Order book, naive -> final | 147.5 -> 53.0 ns per event |
| Order-table prefetch, 48 MiB table | -37% per event |

## Known limits

One writer per bus; one lost packet stops book updates (no gap recovery yet); the price window
of each instrument is fixed at startup; Apple Silicon macOS only. More: `DESIGN.md` §9-10.
