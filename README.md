# mdbus

A shared-memory market-data bus in C++20, header-only, that I built and measured on an M1
MacBook Air. One writer process publishes book deltas and trades into a broadcast ring, and any
number of reader processes consume it without locks; the writer never waits for a reader
(though a reader still costs it time, measured below). An exchange simulator and an L3 feed
handler sit in front, so the path from UDP datagram to consumer book runs for real. The design
and every measurement are in `DESIGN.md`.

## Where things are

Start with the six files in **bold**, in order: they are the whole bus. Each file holds what
its name says.

Folders: `include/mdbus/` is the bus, `include/mdbus/book/` is the feed handler's order book,
`include/mdbus/feed/` is the exchange feed's wire format and sockets.

| File | What it holds |
|---|---|
| **`include/mdbus/constants.hpp`** | Every compile-time constant: slot count, payload words, cache-line sizes, timings |
| `include/mdbus/status.hpp` | `Status` error codes (the library never throws), `status_name`, `check_or_abort` |
| `include/mdbus/clock.hpp` | The two clocks: 24 MHz hardware ticks for latency, steady_clock ns for heartbeats |
| `include/mdbus/atomic_policy.hpp` | `StdAtomics`: the shared word type and its six load/store/fence operations |
| `include/mdbus/wait_policy.hpp` | What a reader does when nothing is ready: `SpinWait` or `SleepWait` |
| `include/mdbus/messages.hpp` | The three bus messages and the `Schema` typelist that checks and dispatches them |
| **`include/mdbus/ring_slot.hpp`** | One ring slot: stamp, `MessageHeader`, payload words, the payload checksum |
| `include/mdbus/control_block.hpp` | The block at offset 0 of a segment: identity + ready marker, writer liveness, head hint |
| **`include/mdbus/ring.hpp`** | **The protocol**: `RingWriter::publish` and `RingReader::try_poll` |
| **`include/mdbus/snapshot_table.hpp`** | Per-instrument recovery snapshots: one seqlock record each, `SnapshotTable` |
| `include/mdbus/bus_layout.hpp` | `BusLayout` (a bus's compile-time shape) and `LayoutHasher` (the layout hash) |
| `include/mdbus/segment_format.hpp` | Where each part sits inside the shared-memory segment; build and validate it |
| `include/mdbus/shared_memory.hpp` | `SharedMemoryMapping`: one mmap of shared memory, pre-faulted and locked |
| `include/mdbus/bus_paths.hpp` | Bus name rules, the shm name and lock-file path, `destroy_bus` |
| `include/mdbus/writer_liveness.hpp` | `WriterLock` (one writer per bus) and `WriterHealthMonitor` (Alive / Stalled / Down) |
| **`include/mdbus/publisher.hpp`** | `Publisher`: encode a message, publish it, keep the instrument's snapshot current |
| `include/mdbus/bus_writer.hpp` | `BusWriter`: create and own a named bus, replacing any old segment |
| `include/mdbus/bus_reader.hpp` | `BusReader`: open and validate a named bus read-only |
| `include/mdbus/instrument_recovery.hpp` | `InstrumentRecoveryTable`: per instrument, stale or not, and the first seq to deliver |
| **`include/mdbus/consumer.hpp`** | The CRTP `Consumer`: poll, dispatch to `on()` handlers, lazy recovery, writer health |
| `include/mdbus/book/` | Order events, top levels, price ladder, order table, instrument lookup, `FeedBook`, publishing its output |
| `include/mdbus/feed/` | `wire_format.hpp` (ITCH-shaped packets, encode/decode, gap tracking), `multicast_socket.hpp` |
| `src/` | `mdbus_feed_handler` (the bus's writer), `mdbus_watch` (the shortest complete reader) |
| `sim/` | Stands in for the exchange: `mdbus_exchange_sim` and its seeded order-event generator |
| `baseline/` | Never part of the system, only measured and checked against: the old book and the ladder F0..F4 to the final one, the bus variants, the mutex ring |
| `bench/` | `bus_bench.cpp`, `feed_bench.cpp`, `dispatch_bench.cpp`, the workload, measurement and result-row helpers, `decisions.yaml` (predictions) |
| `tests/` | Unit tests, the book oracle, end to end over UDP, failure injection with real processes, the mutant stress; shared code in `test_harness.hpp` and `test_helpers.hpp` |
| `scripts/` | `compare.py` (paired comparisons), `report.py`, `check.sh` (sanitizers), `demo.sh` |

Everything in `include/mdbus/` is part of the system. The few hooks there that exist only for
tests or benchmarks say so in the first line of their comment.

## How it is checked

- `tests/mutants.hpp`: five mutants, four that each weaken one memory-ordering operation of the
  real ring and snapshot code (through the atomic policy) and one that skips the reader's
  re-check. `tests/mutant_stress.cpp` runs each on hardware, on P and on E cores, and must kill
  every one while the unmodified control survives.
- `tests/failure_test.cpp`: a writer killed mid-slot and replaced, killed mid-snapshot, stopped;
  a second writer; dead readers. Each with a deliberate break that its check must catch.
- `tests/book_vs_naive_test.cpp`: the final book's top K must match a plain `std::map` model
  after every event, and every step of the ladder must match the old book byte for byte.
- `tests/no_alloc_test.cpp`: a counting operator new shows the hot paths never allocate.

## How to run

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
    ctest --test-dir build -j4      # 31 tests: about 10 s
    scripts/check.sh                # the same under ASan+UBSan and TSan
    scripts/demo.sh 10              # live: simulator -> feed handler -> bus -> viewer, 10 s

One benchmark run, one paired comparison, all of them, and the report:

    ./build/bench/mdbus_bench_Base --duration-ms 3000 --out /tmp/base
    python3 scripts/compare.py Copy --out /tmp/raw
    python3 scripts/compare.py --all          # about 45 min, into results/raw, then the report
    python3 scripts/report.py results

Each run writes `row.csv` and prints a summary: the fast reader's hop (body mean, mean over all
messages, p50, p99, p99.9; intervals under 3 ticks), e2e p99 and `conditions`. A busy machine
never fails a run: it completes, and `conditions` lists the gates it failed (page faults,
P-core share, clock). `compare.py` re-runs such runs up to twice, then leaves their pair out
and counts it.

**In an interview** (built beforehand; times measured on this machine):

| Show | Command | Time |
|---|---|---|
| The pipeline, live | `scripts/demo.sh 10` | 11 s |
| One benchmark run | `./build/bench/mdbus_bench_Base --duration-ms 1000 --out /tmp/b` | 1.3 s |
| The slow-reader effect | the same with `--slow read` | 1.3 s |
| A paired comparison, live | `python3 scripts/compare.py Copy --duration-ms 500 --out /tmp/live` | 10 s |
| Every mutant killed on hardware | `./build/mutant_stress` | 4 s |
| A writer killed mid-slot, then replaced | `./build/failure_test writer_killed_mid_slot_then_replaced` | < 1 s |
| UDP to consumer, end to end | `./build/end_to_end_test` | < 1 s |
| Every test | `ctest --test-dir build -j4` | 10 s |

Requires macOS 14.4+ on Apple Silicon, CMake 3.25+, network once (HdrHistogram), and PyYAML.

## Results

Six counterbalanced pairs of 3 s runs per comparison, all six agreeing (full method, every
result and what each taught me in `DESIGN.md`; the raw table in `results/report.md`). These
were measured before the 2026-10-04 simplification; the re-run is pending.

| Comparison | Result |
|---|---|
| Fast reader hop, P-core to P-core, 1 M msg/s | 75.9 ns body mean (91 ns over all messages); p99 3 ticks |
| Payload in one 64 B line vs two | 75.7 vs 107.2 ns |
| Readers poll a global head instead of the slot stamp | 75.9 -> 108.5 ns |
| Slow E-core reader present vs absent, on the fast reader | 75.9 -> 195.7 ns |
| E-core process that never maps the ring, instead | no difference |
| Same slow reader moved behind a P-core relay ring | 188.9 -> 65.1 ns |
| Writer per publish, alone vs one caught-up reader | 3.4 -> 27.4 ns (same 49.6 instructions) |
| Feed stage, my old book design -> final | 162.2 -> 33.0 ns per event |
| Order-table prefetch, 32 MB table | -40% per event |
| Mutex over the same slots -> ring, e2e p99, slow reader present | 143 us -> 375 ns |

## Known limits

- One writer per bus.
- A restarted writer starts a fresh bus; readers re-attach with every instrument stale.
- A bus left in shared memory by a build from before 2026-10-05 has a different ready marker:
  remove it once with `./build/mdbus_watch --bus NAME --destroy`.
- Recovery is lazy only, and TCP gap recovery is not built, so `kInstrumentSuspect` stays raised for the
  session.
- Apple Silicon macOS only.
