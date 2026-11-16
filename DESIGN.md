# mdbus: Design

Repository: https://github.com/shashankk-10/mdbus

## 1. Aim

- Send market data from one writer process to many reader processes on one machine, through
  shared memory (called a "bus").
- Low latency, no locks. The writer never waits for a reader, though readers do slow it down.
- C++20 header-only library. Apple Silicon macOS, measured on an M1 MacBook Air (4 P-cores in
  one cluster, 4 E-cores, 8 GB), macOS 15.6, Apple clang 17.0.0.

Results in short:

- Writer -> reader: 56 ns trimmed average (107 ns over all messages, OS stalls included), p99
  125 ns, at 1 M msg/s.
- Order book: 56.8 ns per event, down from 165.2 ns for a naive version.
- With a slow reader attached, the fast reader's p99 is 375 ns. Same ring behind a mutex: 124 µs.
- Lost packet or refused order: affected books are flagged and readers stop updating them.

## 2. Problem Statements

Trading words: **instrument** = one tradable symbol, **order book** = all its live buy and sell
orders, **level** = one price on one side with the total qty waiting there.

1. **Fast fan-out:** one writer, many readers. A reader that keeps up gets every message, in
   order. No locks, system calls or heap allocation per message.
2. **Readers at different speeds:** a slow reader never blocks the writer. A reader that falls
   behind recovers by itself (books come back, skipped trades are lost).
3. **A real input path:** order events over UDP, a full order book, publish only top-level
   changes.
4. **Failures:** writer crash or restart, second writer, lost packets, bad input. If a book
   might be wrong, flag it and stop applying updates (fail closed).

## 3. Architecture

### 3.1 Components

- **Exchange simulator:** repeatable stream of order events in numbered UDP packets.
- **Feed handler** (the only writer): keeps the order book, publishes what changed.
- **Consumers** (any number of readers): read the bus, get one callback per message.

### 3.2 Links between them

- **Exchange -> feed handler:** UDP multicast. A lost packet = a gap in sequence numbers.
- **Feed handler -> consumers:** shared memory. A ring of 16,384 slots (about 16 ms of history
  at 1 M msg/s) + one snapshot per instrument. Readers never write to it.

### 3.3 Call journey

```
EXCHANGE SIMULATOR   make order event -> pack into packet -> send over UDP
        |
FEED HANDLER         receive packet -> decode -> check sequence number
(only writer)        for each event: update book, find what changed at the top
                       trade:            write one ring slot
                       delta or status:  write one ring slot, then the snapshot
        |  shared memory
CONSUMERS            poll next slot -> copy if complete -> recovery check
(many readers)       -> call the user's handler for that message type
```

The writer never reads anything a reader writes. It doesn't even know how many readers exist.

## 4. Components

### 4.1 Exchange simulator

- Keeps about 16K live orders around a fixed reference price (bids below, asks above, so they
  never cross). Same seed, same stream.

### 4.2 Wire format and UDP

- Packet = header (first message number, count) + up to 10 messages. Malformed = dropped whole.
- First message number ahead of expected = gap (messages lost). Behind = duplicate, dropped.

### 4.3 Feed handler and order book

- Checks for packets without blocking. Heartbeat when idle.
- Book keeps every live order, plus the best 6 levels per side:
    - **Order id -> order:** hash table in one fixed array, refuses new orders at 75% full.
    - **Price -> qty:** plain array over 4,096 prices around the reference price. No tree.
    - **Refilling the 6th level:** a bitmap of prices with orders. At most two bit scans.
    - **Delta:** compare the top 6 before and after an event. At most 3 levels change.
- Bad add (price outside window, table full, duplicate id): refused, instrument marked "bad".

### 4.4 Messages

- Three types: `BookDelta` (top levels changed), `Trade`, `InstrumentStatus` ("bad" or
  "suspect"). Rules checked at compile time.

### 4.5 Shared-memory layout

- Control block, then the ring (16,384 x 128 B), then one 128 B snapshot per instrument.
- Pages are locked in RAM up front, so no page faults while publishing or reading.

### 4.6 Ring buffer

- Slot = 128 B: 8 B stamp + 56 B message (together one 64 B unit) + 64 B left empty.
- Seqlock per slot: writer makes the stamp odd, writes the message, makes it even. Reader
  copies, then checks the stamp again. Changed = copy thrown away.
- The stamp also carries the message number, so a reader knows if the slot was reused.
- Wait-free on both sides: no retries, no compare-and-swap, readers write nothing.

### 4.7 Snapshot table

- One 128 B record per instrument: top 6 levels per side, flags, and the newest ring message
  already in it. 6 levels is simply what fits in 128 B.
- Same seqlock idea. A reader retries (max 256 times), since any newer snapshot is fine.

### 4.8 Publisher

- Trade: one ring slot. Delta or status: ring slot, then the instrument's snapshot.
- Heartbeat every 1 ms, and "Exited" on clean shutdown.

### 4.9 Consumer

- The user writes one `on()` per message type. CRTP makes the call direct and inlined.
- Waiting: `SpinWait` (lowest latency, keeps a core busy) or `SleepWait` (sleeps 1 ms).

### 4.10 Writer lock and health

- One writer per bus via an exclusive file lock, freed by the OS if the writer crashes.
- Readers judge the writer Alive or Down from its heartbeat.

## 5. Problem Scenarios Handling

- **5.1 A reader falls behind (lapped):** it sees a stamp higher than expected, jumps to the
  newest messages and marks every instrument stale (same as a new reader).
- **5.2 Recovery after a lap:** on the next delta for a stale instrument, read its snapshot and
  skip deltas already in it. Cost: skipped trades are lost, and a quiet instrument stays stale
  until its next delta.
- **5.3 A torn read:** the re-check sees a new stamp. The mixed copy is never delivered.
- **5.4 The writer dies in the middle of a write:** the stamp stays odd, readers wait or give
  up, and report the writer Down once the heartbeat stops.
- **5.5 The writer restarts:** it creates a fresh segment and readers attach again. Books don't
  come back by themselves: the new handler joins mid-stream, sees a gap and marks everything
  suspect. Trades come back only for new orders.
- **5.6 Is the writer alive?** Alive if a message or heartbeat came in the last 100 ms, else
  Down (at once on a clean exit).
- **5.7 A second writer:** the lock fails at once.
- **5.8 A lost packet:** every instrument goes "suspect" (same for a malformed or swapped
  packet). Book updates stop, trades still flow, and it never clears since there is no gap
  recovery yet. Fail closed: a reader never applies updates to a flagged book.
- **5.9 Bad input to the book:** a refused add marks the instrument "bad". Its status goes out
  before any delta of that event. An unknown order id is counted and ignored.
- **5.10 A reader built differently, or a buggy reader:** a layout mismatch is refused at
  attach. Readers map the bus read-only, so a buggy one can't corrupt it or block the writer.
  At worst it slows the others down.

## 6. Core Design Decisions

| Problem | Choice | Instead of |
|---|---|---|
| Deliver to many readers | Ring, seqlock per slot | Mutex: writer waits on a slow reader |
| Slow reader | Writer overwrites, reader detects the lap and recovers | Back-pressure: one slow reader stalls everyone |
| Detect a lap | Message number inside the slot stamp | Plain version counter: can't tell laps apart |
| Rebuild a book after a lap | One snapshot per instrument | Replay: old messages already overwritten |
| Order id -> order | Open-addressing hash table | `std::unordered_map`: heap node per order |
| Price -> qty | Array over a fixed price window | `std::map`: tree walk |

## 7. Optimizations

- **CRTP instead of virtual calls:** 9.9 fewer instructions per message.
- **One 64 B unit per message:** spreading it over two units costs 49 ns more.
- **Poll the slot stamp, not a global counter:** 42 ns saved (median; 30 to 52 ns over the 5
  usable pairs, one pair short of a verdict).
- **128 B slots:** with 64 B slots two slots share a 128 B cache line, and here they cost 7.2 ns
  more per hop at 1 M msg/s and 6.1 ns more per publish at saturation.
- **Prefetch 4 events ahead:** -33% with a 48 MiB order table. At the default size it fits in
  cache and gains too little to count.

## 8. Measurement

### 8.1 The clock

- CPU counter at 24 MHz (1 tick = 41.67 ns), the same in every process.
- Latency averages are good to about one tick, so changes that small (42, 49 ns) are rough.
  Per-publish and per-event costs are timed in batches, so the tick doesn't limit them.

### 8.2 Method

- Latency runs: fixed 1 M msg/s. Throughput runs: as fast as possible.
- Percentiles use every sample. "Average" drops samples over 667 ns (about 0.25%, OS stalls).
- A vs B: 6 pairs of runs. A change counts only if all 6 agree and it beats both run-to-run
  noise and a set minimum (1 ns; 1 tick for p99; 3% for the order book). Busy-machine runs are
  redone up to twice, then the pair is dropped.
- This machine was also running other work, and the gates rejected 92 of 216 runs in the first
  pass. `compare.py` was run twice more over the same runs, which re-ran only the rejected ones:
  215 of 216 ended valid (`results/campaign.log`). The A medians of one metric therefore come
  from different times: the Base hop's median ranges from 54 to 66 ns across comparisons.

### 8.3 Results

| What | Result |
|---|---|
| Writer -> reader at 1 M msg/s | Trimmed average 56 ns (107 ns over all messages), p99 125 ns, p99.9 9.1 µs, max 256 µs in a typical run, 9.2 ms worst |
| Writer cost per publish | 2.31 ns alone, 24.4 ns with one reader (same 36.65 instructions, about 10x the time: most likely cache traffic) |
| Max rate with one reader | About 44 M msg/s. The fast reader was lapped in 26 of 30 runs and lost a median 0.16% of messages, 10% at worst |
| Fast reader, slow reader added | 66 -> 219 ns (3.3x slower) |
| Mutex vs ring, slow reader, 1 M msg/s | p99 124 µs vs 375 ns. Mutex writer behind schedule on 71% of publishes (ring 0.1%) |
| Mutex vs ring, flat out | Mutex 29.3 ns per publish, ring 3.7 ns less (5 of 6 pairs usable: unresolved). The mutex reader lost about 61% of messages |
| Order book, naive -> final | 165.2 -> 56.8 ns per event (price array -34 ns, hash table -61 ns, id instead of symbol -6.4 ns) |

- Medians over runs. Slow reader: efficiency core, 500 ns of work per message, busy-waits
  between messages, lapped 1-3 times in 6 of 24 runs.
- p99.9 and max are most likely OS stalls. Same build: back-to-back runs differ by up to 3.8 ns,
  all 36 runs span 53-76 ns.

### 8.4 How correctness was validated

- **Memory ordering:** 5 deliberately broken ring versions run on real cores. Tests catch each.
- **End to end:** real processes over UDP, books match. Dropped packet, killed writer, second
  writer tested.
- **Other:** order book checked against a simple `std::map` model after every event, recovery
  tests, no allocation on hot paths, sanitizers.

### 8.5 How hypotheses were validated

- Expected ranges were written down before the runs and never changed: 5 of 14 hit. Caveats:
  three were written for older code, and four are narrower than one tick.
- Key miss: the design claim that a slow reader costs the fast one nothing. Result: 3.3x slower.
- Also missed: 64 B slots were predicted to cost nothing on P-cores. Here they cost 6 to 7 ns.

## 9. Known Limitations

- **One lost packet stops book updates** for the rest of the run (no gap recovery). Same after a
  malformed packet or a writer restart.
- **Readers slow each other:** a slow reader takes the fast one from 66 to 219 ns, and one
  reader makes the writer ~10x slower. Only 1 fast + 1 slow reader measured so far.
- **Fail closed covers books only.** After a gap some trades go missing, unmarked. An exchange
  restart isn't detected: its packets are dropped as old, then applied to the old book unflagged.
- **Fixed 4,096-price window** per instrument, and readers get only the top 6 levels.
- **Not measured:** UDP -> reader latency, feed handler throughput, recovery time.

## 10. Future Scope

- **Gap recovery:** resend missing packets over TCP, then clear "suspect".
- **Isolate slow readers** behind a relay process with its own ring.
- **Measure more:** 2, 4 and 8 readers, the full UDP -> reader path, and Linux with each
  process pinned to its own core.
