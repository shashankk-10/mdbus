# mdbus: design

A shared-memory market-data bus in C++20, built and measured on an M1 MacBook Air.

## 1. The problem

- Market data must reach many strategy processes on one machine: every message, in order.
- Readers run at different speeds. A slow reader must never stop the writer.
- My two earlier projects each solved half of this:
  - SPSC queue: every message, but to one reader, and that reader can block the producer.
  - Seqlock: many readers, but only the latest value, no history.
- mdbus combines them: a broadcast ring (every message, many readers) plus per-instrument
  seqlock snapshots (to recover after falling behind).

## 2. Goals and non-goals

**Goals**
- One writer process, any number of reader processes.
- The writer never waits for a reader and does not know how many readers exist.
- A reader that falls too far behind detects it exactly and recovers on its own.
- No locks, no system calls and no heap allocation on the hot path.
- Every design choice measured against its alternative.

**Non-goals**
- Lossless delivery to every reader (a dead reader would then stall the writer).
- More than one writer per bus.
- Portability: Apple Silicon macOS only.

## 3. Architecture

```
mdbus_exchange_sim --UDP multicast--> mdbus_feed_handler --shared memory--> readers
 (fake exchange)      ITCH-shaped        (FeedBook + Publisher)   (ring)     (Consumer)
```

- **Exchange simulator** (`sim/`): sends seeded order events (add, cancel, execute, replace)
  as sequenced UDP packets.
- **Feed handler** (`src/feed_handler.cpp`): decodes packets, applies each event to its order
  book (`FeedBook`), and publishes what changed in the top 6 levels, plus trades.
- **Bus** (`include/mdbus/`): one shared-memory segment per bus, holding a control block, the
  ring and a snapshot table.
- **Readers** derive from the CRTP `Consumer` and get typed `on()` callbacks.
- The book, the network and the simulator are supporting stages. The bus is the core.

## 4. Shared-memory layout

One POSIX shared-memory segment per bus, created by the writer, mapped read-only by readers.

| Part | Size | What it holds |
|---|---|---|
| `ControlBlock` | 3 x 128 B | ready marker + layout hash; writer heartbeat + state; head hint |
| Ring | 16384 slots x 128 B = 2 MB | the messages |
| Snapshot table | one 128 B record per instrument | latest top-6 book of each instrument |

**Ring slot** (`ring_slot.hpp`)
- 8 B stamp + 7 x 8 B payload words = exactly 64 B, padded to 128 B.
- Payload = 24 B `MessageHeader` (latency tick, publish tick, instrument id, type id,
  checksum) + 32 B message body.
- Why 64 B of data: a reader then moves one 64 B line per message.
  - Measured: a payload spread over two lines costs 75.7 -> 107.2 ns per hop.
- Why pad to 128 B: macOS reports a 128 B line (`hw.cachelinesize`), and across the two CPU
  clusters sharing happens at 128 B.
  - Measured cost: 3 ns per publish at full speed. I keep it as a hedge, because macOS will
    not let me pin a reader to one cluster.

**Control block** (`control_block.hpp`)
- Each part has its own 128 B line, because each is written at a different rate (once, every
  1 ms, every 64 publishes). This avoids false sharing.
- The ready marker (bytes "MDBUSRDY") is stored last, with release. A reader that sees it
  knows the segment is fully built.
- The layout hash covers slot count, payload size, every message's id and size, and a
  version. A reader built differently refuses to attach (`Status::LayoutMismatch`).

**Messages** (`messages.hpp`)
- `BookDelta` (up to 3 changed levels on one side), `Trade`, `InstrumentStatus`.
- Plain structs with padding written out, so a checksum over them is deterministic.
- `Schema<Messages...>` is a typelist; fold-expression `static_assert`s check each message
  is trivially copyable and has a unique non-zero id.

## 5. The ring protocol

Stamp of a slot holding message `seq`: `2*seq + 1` while being written, `2*seq + 2` when done.

**Writer** (`RingWriter::publish`, `ring.hpp`)
1. Store the odd stamp `2*seq + 1` (relaxed), then a release fence.
2. Store the 7 payload words (relaxed).
3. Store the even stamp `2*seq + 2` with release.
4. Every 64 publishes, store the head hint.

**Reader** (`RingReader::try_poll`)
1. Load the stamp with acquire.
   - Less than `2*seq + 2`: not written yet.
   - More than `2*seq + 2`: the writer lapped this reader.
2. Copy the 7 words (relaxed), then an acquire fence.
3. Load the stamp again. Unchanged: the copy is good. Changed: the copy is torn, report a lap.

**Why these choices**
- **Sequence inside the stamp:** a plain seqlock counter cannot tell "holds seq" from "holds
  seq + 16384". Encoding seq makes lap detection exact.
- **Poll the slot, not a global head:** with a head, a reader waits for the head line, then
  fetches the slot line: two transfers in a row. Measured: +32.6 ns per hop.
- **Acquire, not seq_cst:** they differ only in waiting for this thread's earlier release
  stores, and a reader makes none (`ldapr` vs `ldar` on arm64).
- **No retry on a torn copy:** the slot only changes again for `seq + 16384`, so `seq` is gone.

**Progress guarantees**

| Operation | Guarantee |
|---|---|
| `publish` | Wait-free: straight-line stores, never reads anything a reader writes |
| `try_poll` | Wait-free: one attempt, three outcomes, writes no shared memory |
| Snapshot read | Blocking, with a cap: a writer stopped mid-update makes readers give up after 257 attempts |

- Wait-free bounds steps, not time. Measured: the same 49.6 instructions per publish take
  3.4 ns alone and 27.4 ns with one caught-up reader, because the reader's copy of the line
  must be taken back before each write.

## 6. Recovery after a lap

Lazy, per instrument (`consumer.hpp`, `instrument_recovery.hpp`):
1. A lapped reader jumps to the head hint and marks every instrument stale.
2. On the next `BookDelta` for a stale instrument, it reads that instrument's snapshot and
   hands it to `on_snapshot`.
3. After that, deltas with `seq <= last_included_seq` are skipped; newer ones are delivered.

- **Trades are never skipped:** no snapshot contains them.
- **Why it is correct:** the writer updates a snapshot right after the slot that changed it,
  before the next slot. So a reader that has seen slot `seq` reads a snapshot of version
  `seq - 1` or newer.
- **Why lazy, not eager:** reading all snapshots before resuming can take longer than the
  ring's history, so the reader would be lapped again forever.
- A new reader starts the same way: at the head hint, everything stale.

## 7. Failure handling

**One writer per bus**
- The writer holds an exclusive `flock` on a lock file in `/var/tmp/mdbus-<uid>/`.
- Not `/tmp`: macOS deletes old files there, even ones in use.
- A second writer gets `Status::AnotherWriterRunning`.

**Is the writer alive?** (`writer_liveness.hpp`)
- The writer stores a heartbeat every 1 ms.
- A reader that has seen nothing for 100 ms checks, cheapest first:
  1. the writer's state word (exited or replaced);
  2. the heartbeat's age;
  3. the flock. The kernel releases it when the writer dies (even on SIGKILL) and keeps it
     while the writer is stopped. So the reader tells **Down** from **Stalled** without
     trusting a PID.

**Writer restart**
- A restarted writer always creates a fresh segment. It marks the old one `Replaced`.
- Readers see that, re-attach by name, and recover as after a lap.
- I first built restart-in-place. I removed it: readers had to mark everything stale anyway,
  and it was the hardest code in the repo.

**Readers cannot hurt the bus**
- Readers map the segment read-only, so a buggy reader faults instead of corrupting it.
- Readers write no shared memory, so a dead reader costs the writer nothing.

**Waiting** (`wait_policy.hpp`)
- `SpinWait` for the fast reader (lowest latency, holds a core).
- `SleepWait` (1 ms) for slow readers. The writer is not involved either way.

## 8. Feed path

**Network** (`feed/wire_format.hpp`, `feed/multicast_socket.hpp`)
- UDP multicast on loopback: one-to-many, and loss shows up as a sequence gap.
- Why not TCP: one lost segment would hold back everything behind it, and a slow receiver
  would push back on the sender.
- Messages are ITCH-shaped: packed per-type structs, big-endian, length-prefixed, in
  sequenced packets. 32.5 B per event on average.
- A gap marks every instrument `kInstrumentSuspect`; consumers then treat snapshots as
  unusable. Gap recovery over TCP is not built.

**Order book** (`book/feed_book.hpp`)

| Sub-problem | Old design | Final design | Cost |
|---|---|---|---|
| Instrument lookup | hash map on symbol string | id is the array index | O(1) |
| Order id -> order | `unordered_map` (a heap node per add) | open-addressing table, 24 B entries, 75% load cap | O(1) expected |
| Price levels | `std::map` | flat array over a 4096-price window | O(1) |
| Next best price | `map::begin` | two-level bitmap + bit scan | 2 scans |
| Output | whole book | diff of the top 6 levels, at most 3 entries | O(1) |

- Everything is allocated at startup. A counting `operator new` test proves the event path
  never allocates.
- Out-of-window prices and a full order table fail closed: the instrument is marked bad,
  nothing goes to the heap.
- `baseline/book_ladder.hpp` builds the book in five steps (F0..F4), each changing exactly
  one policy, so each step is measured on its own.

## 9. Code structure

- **Templates on policies:** the ring and snapshot code is written once against an atomic
  policy. Production uses `StdAtomics`; tests plug in weakened policies (mutants).
- **CRTP `Consumer`:** handlers are called directly, no virtual call. Measured: a virtual
  handler costs +9.9 instructions per message.
- **Fold-expression dispatch:** one compare per message type, then a direct call.
- Reading order: `constants.hpp` -> `ring_slot.hpp` -> `ring.hpp` -> `snapshot_table.hpp` ->
  `publisher.hpp` -> `consumer.hpp`. The README has the full file map.

## 10. Measurement method

- **Clock:** the 24 MHz hardware counter (`isb; mrs cntvct_el0`), the same in every process.
  One tick = 41.67 ns, so latencies under 3 ticks are given as tick counts.
- **Hop:** writer's clock read before publishing -> fast reader's validated copy.
- **e2e:** from the message's scheduled send time, so a late writer is charged for being late
  (avoids coordinated omission).
- **Runs are the unit:** 6 counterbalanced pairs (AB, BA, ...) of 3 s runs per comparison;
  median paired difference; exact sign test; an A/A run gives the noise floor (about 0.5 ns).
- **Gates on what the run measured:** zero page faults, at least 99% of cycles on P-cores,
  clock at least 2.8 GHz, no clock-order violations. A failed run is re-run up to twice.
- **Predictions first:** each prediction is written in `bench/decisions.yaml` before the
  comparison runs. 8 of 17 hit.

## 11. Results

M1 MacBook Air, Apple clang 17, `-O2`. These were measured before the 2026-10-04
simplification and have not been re-run since. The 2026-10-05 readability pass left the
generated code of every hot path instruction-for-instruction identical.

| Comparison | Result |
|---|---|
| Fast reader hop, P-core to P-core, 1 M msg/s | 75.9 ns mean; p50 2 ticks, p99 3 ticks |
| Payload in one 64 B line vs two | 75.7 vs 107.2 ns |
| Poll a global head instead of the slot stamp | 75.9 -> 108.5 ns |
| Slow E-core reader present vs absent | 75.9 -> 195.7 ns |
| Same slow reader behind a P-core relay ring | 188.9 -> 65.1 ns |
| Writer per publish: alone vs one caught-up reader | 3.4 -> 27.4 ns |
| Mutex over the same slots -> ring, e2e p99 | 143 us -> 375 ns |
| Feed stage: old book -> final book | 162.2 -> 33.0 ns per event |
| Order-table prefetch, 32 MB table | -40% per event |

**What I learned**
- **Wait-free is not isolation.** A slow reader on an E-core makes the fast reader 2.6x
  slower, though the writer never waits for it. An E-core process that does not map the ring
  changes nothing, so the cost is the shared cache lines.
- **Fix: a relay.** A P-core relay copies the ring into a second ring for slow readers. The
  fast reader goes back to 65 ns. Cost: one P-core.
- **A mutex looks cheap only because it starves readers.** Its writer cost was similar, but
  its reader lost 98 M of 114 M messages. Compare tails: 143 us vs 375 ns at p99.

## 12. Verification

- **Mutants on real hardware:** 5 mutants, each weakening one memory-ordering operation of
  the real code (or skipping the reader's re-check). `mutant_stress` runs a writer and 3
  readers on P and E cores. Every mutant is caught; the unmodified code passes.
- **Failure tests with real processes:** writer killed mid-slot then replaced, killed
  mid-snapshot, stopped, a second writer, dead readers. Each test has a deliberate break that
  proves its check fires.
- **Book oracle:** the final book must match a plain `std::map` model after every event.
- **Sanitizers:** the suite passes under ASan + UBSan and TSan. TSan cannot see a missing
  fence between atomics, so the mutants are the real evidence for the protocol.
- `ctest`: 31 tests, about 10 s.

## 13. Limits and next steps

**Known limits**
- One writer per bus. Recovery is lazy only.
- A packet gap leaves instruments suspect for the session (no TCP gap recovery).
- Loopback multicast measures the macOS socket stack, not a real NIC.

**Considered, not built**
- Lossless delivery: a dead reader would stall the writer.
- RCU snapshots: across processes, readers would have to write epochs into shared memory.
- Huge pages: macOS on Apple Silicon offers none; the 2 MB ring is only 128 pages.
- Thread pinning: macOS refuses it. I use QoS classes and check the P-core share instead.

**Open questions**
- Whether the 128 B padding ever pays off on this machine.
- How the numbers move on x86 Linux, where the memory model is stronger and lines are 64 B.
