# mdbus: Design

Repository: https://github.com/shashankk-10/mdbus

## 1. Aim

Deliver market data from one writer process to many reader processes on the same machine,
through shared memory, with low latency, no locks, and a writer that never waits for a reader.

- Language: C++20, header-only library.
- Platform: arm64 (Apple Silicon), macOS.
- Measured on: Apple M1 Pro (8 performance cores in two clusters, 2 efficiency cores),
  Apple clang 15, `-O2`.

## 2. Problem Statements

1. **Fast fan-out**
   - One writer, many readers; every reader gets every message, in order.
   - No locks, system calls or heap allocation when publishing or reading a message.
2. **Readers at different speeds**
   - A slow reader must never block the writer.
   - A reader that falls too far behind must detect it and recover by itself.
3. **A real input path**
   - Receive exchange order events over UDP, keep a full order book, and publish only what
     changed at the top of the book.
4. **Failures**
   - Handle a writer that crashes or restarts, a second writer, lost packets and bad input.

## 3. Architecture

### 3.1 Components

1. **Exchange simulator**
   - Generates a repeatable stream of order events: add, cancel, execute, replace.
   - Packs them into numbered packets and sends them over UDP multicast.
2. **Feed handler** (the only writer)
   - Receives packets and checks their sequence numbers.
   - Applies each event to its order book.
   - Publishes what changed (top-level updates, trades, status) to the bus.
3. **Consumers** (any number of readers)
   - Map the bus read-only and poll it.
   - Get one typed callback per message.
   - Detect when they fall behind and recover by themselves.

### 3.2 Links between them

- **Exchange -> feed handler: UDP multicast.** One sender, many receivers. A lost packet shows
  up as a gap in sequence numbers.
- **Feed handler -> consumers: shared memory.** One segment per bus: a ring of messages plus
  one snapshot per instrument. Readers never write to it.

### 3.3 Call journey

Data flows top to bottom. An indented line is called by the nearest less-indented line above it.

```
EXCHANGE SIMULATOR
  OrderEventGenerator::next_event()      // make one order event
  PacketBuilder::add()                   // put it in the current packet
  MulticastSender::send()                // send the packet
                 |
                 |  UDP multicast: numbered packets
                 v
FEED HANDLER (the only writer)
  MulticastReceiver::try_receive()       // read one packet, if any
  decode_packet()                        // bytes -> order events
  SequenceGapTracker::check_packet()     // find lost or repeated packets
  FeedBook::apply_batch()                // for each event in the packet:
    FeedBook::apply()                    // update the book, return changes
    publish_book_output()                // send the changes to readers
      Publisher::publish()               // a trade: ring slot only
      Publisher::publish_and_update_snapshot()  // a delta or a status:
        RingWriter::publish()            // 1. write the next ring slot
        SnapshotTable::write()           // 2. save the instrument's levels
                 |
                 |  shared memory: ring + snapshots (read-only for readers)
                 v
CONSUMER (any number of readers)
  Consumer::poll_once()                  // try to read the next message
    RingReader::try_poll()               // copy the slot if it is complete
    Schema::dispatch()                   // type id -> the message's type
      filter_and_deliver()               // recovery (5.2): deliver or skip
        MyReader::on()                   // the user's handler (4.9)
```

The writer never calls a reader and never reads anything a reader writes. It does not even
know how many readers exist.

## 4. Components

The code below shows the basic shape of each part, not the full implementation.

### 4.1 Exchange simulator

- Keeps a steady number of live orders, so the order book works at a stable size: below the
  target it adds an order, above it it ends one, at the target it picks at random.
- Puts every bid below and every ask above a fixed reference price per instrument, so they
  never cross (the simulator does not match orders).
- The same seed always gives the same stream, so runs are repeatable.

### 4.2 Wire format and UDP

- A packet is a header plus up to M messages.
- Every message is sent behind a 2 B length, so a receiver can skip a type it does not know.
- Fields are big-endian and packed (no padding). The decoder checks each length before it
  reads, and drops a malformed packet.
- `first_seq` in the header lets the receiver find gaps and duplicates (sequence check below).

```cpp
#pragma pack(push, 1)        // no padding; fields are big-endian
struct PacketHeader {
  uint64_t first_seq;        // number of the packet's first message
  uint32_t message_count;    // messages in this packet: 0 to M
  uint32_t end_of_stream;    // 1 on the last packet (it has no messages)
};
// After the header, message_count times: a 2 B length, then the message.

struct AddMessage {          // cancel, execute and replace look similar
  uint8_t  type;             // 'A' for add
  uint16_t instrument_id;
  uint64_t order_id;         // later events name the order by this id
  uint8_t  side;             // 0 bid (buy), 1 ask (sell)
  int32_t  price;            // in whole price steps
  uint32_t qty;              // number of shares
  // also: exchange time, symbol
};
#pragma pack(pop)
```

Sequence check:

```
expected = 1                       // number of the next message wanted

check_packet(first_seq, message_count):
  if first_seq < expected: return DUPLICATE  // a repeat, or arrived late
  if first_seq > expected: result = GAP      // messages in between lost
  else:                    result = IN_ORDER
  expected = first_seq + message_count       // numbers count messages,
                                             // not packets
  return result
```

### 4.3 Feed handler and order book

Main loop:

```
// "publish" writes one ring slot. "+ snapshot" then also saves the
// instrument's top levels, so a reader that fell behind can catch up.
until an end-of-stream packet or a stop signal:
  packet = try_receive()          // next UDP packet, or none (never waits)
  if no packet:
    heartbeat_if_due()            // idle: show readers the writer is alive
    continue
  every 64 packets: heartbeat_if_due()   // busy, but its events may
                                         // publish nothing
  events = decode(packet)
  if malformed: count it, drop the whole packet, continue
  order = check_packet(packet)    // IN_ORDER, GAP or DUPLICATE (4.2)
  if order is GAP (first time only):
    mark every instrument "suspect" for the rest of the run, and
    publish a status message + snapshot for each (5.8)
  if end of stream: stop          // even if this packet is a repeat
  if order is DUPLICATE: drop the packet, continue
  for each event in the packet:   // a GAP packet is applied too
    out = book.apply(event)
    if out has a trade:  publish the trade               // ring slot only
    if out.became_bad:   publish a status message + snapshot
    if out has a delta:  publish the delta + snapshot
on stop: store "Exited", so readers see the writer Down at once
```

The order book keeps every live order and the best K levels of each side:

```cpp
class FeedBook {
 public:
  BookOutput apply(const OrderEvent& event);  // apply one event; return
                                              // what readers must hear
  // Copy one instrument's top K levels and flags into `out`.
  void fill_snapshot(uint16_t instrument, InstrumentSnapshot& out) const;

 private:
  std::vector<InstrumentState> instruments;  // per instrument: top K
                                             // levels per side, flags
  std::vector<PriceLadderSide> ladders;      // one per instrument and side
  OrderTable orders;                         // every live order
};

struct BookOutput {            // what one event changed
  BookDelta delta;             // changed top levels of one side, 0 to 3
  Trade trade;                 // filled in for an execute only
  bool became_bad;             // this event first marked the instrument "bad"
  RejectReason reject_reason;  // why an add was refused, if one was
};

// Hash table in one fixed array: order id -> qty left, instrument, side,
// price. If an id's entry is taken, the order goes in the next free one.
class OrderTable {
 public:
  Index find(uint64_t order_id) const;                // position, or "none"
  bool insert(uint64_t order_id, OrderRecord order);  // false if 75% full
                                                      // or id already there
  void erase(Index position);                         // moves later
                                                      // entries back
};

// One side of one instrument, over a window of W prices around the
// instrument's fixed reference price.
class PriceLadderSide {
  uint32_t qty_at_price[W];         // total qty at each price
  uint64_t nonempty_bits[W / 64];   // 1 bit per price: has orders?
  uint64_t nonempty_words;          // 1 bit per word above: any bit set?

 public:
  uint32_t add_qty(int32_t price, uint32_t qty);     // returns new total
  uint32_t remove_qty(int32_t price, uint32_t qty);  // returns what is left
  // Next price with orders, worse than `price` (lower for bids, higher
  // for asks). Used to refill the top K. At most two bit scans, no loop.
  int32_t next_worse_level(int32_t price, uint32_t& qty) const;
};
```

- One event changes one side of one instrument. The book copies that side's top K levels
  before the change and compares after it. The difference is the delta: at most 3 entries,
  each with the new total quantity (0 means the level is gone).
- A bad add (price outside the window, table full, duplicate id, quantity overflow) is
  refused and the instrument is marked "bad". The book never guesses (fail closed).
- Erase moves later entries back instead of leaving "deleted" markers, so lookups stay short
  over a long run.
- All memory is allocated at startup; applying an event never allocates.

### 4.4 Messages

```cpp
struct MessageHeader {        // first 24 B of every message in the ring
  // Two times for latency tests (8.2), in clock ticks; 0 = not measured.
  uint64_t latency_start;     // e.g. when its packet arrived
  uint64_t publish_time;      // just before the ring write
  uint16_t instrument_id;
  uint8_t  type_id;           // which message follows: 1, 2 or 3
  uint8_t  padding;           // always 0
  uint32_t checksum;          // set on every publish; checked by tests only
};

struct Level { int32_t price; uint32_t qty; };   // qty: new total; 0 = gone

struct BookDelta {            // type 1: changed top levels of one side
  uint8_t side, count, pad[2];   // side: 0 bid, 1 ask; count: 1 to 3
  Level   entries[3];
};
struct Trade {                // type 2: an order in the book was executed
  int32_t price; uint32_t qty;
  uint8_t aggressor, pad[3];     // who traded against it: 0 buyer, 1 seller
};
struct InstrumentStatus {     // type 3: the instrument's flags changed
  uint8_t  flags, pad[3];        // "bad" and/or "suspect"
  uint32_t reject_reason;        // why an add was refused; 0 = none
};

template <class... Messages>
struct Schema {
  // Compile-time checks on every message: copyable as raw bytes, no
  // hidden padding, at most 8 B alignment, a unique type id other than 0.
  // dispatch: finds the type whose id is type_id, reads body as that
  // type, calls handler(message); false if no type matches.
  template <class Handler>
  static bool dispatch(uint8_t type_id, const uint8_t* body,
                       Handler&& handler);
};
// The bus's messages, in dispatch order:
using DefaultSchema = Schema<BookDelta, Trade, InstrumentStatus>;
```

- Padding is written as explicit fields, so every byte of a message is defined.
- `dispatch` is a fold expression: one compare per message type, then a direct call with the
  right type.
- A new message that breaks a rule fails to compile.

### 4.5 Shared-memory layout

```
+---------------------+  offset 0
| Control block       |  3 x 128 B, one line per part:
|                     |    identity:  written once (ready marker,
|                     |               layout version, ring and table sizes)
|                     |    liveness:  about every 1 ms (heartbeat time,
|                     |               writer state: Running or Exited)
|                     |    head hint: every 64 publishes (the next message
|                     |               number; where a new reader starts)
+---------------------+
| Ring                |  N slots x 128 B. N is a power of two, so a
|                     |  message's slot (its number mod N) is a bit mask
+---------------------+
| Snapshot table      |  one 128 B record per instrument
+---------------------+
```

- The writer builds the whole segment, then stores the ready marker last (release store).
  A reader that sees the marker sees a complete segment.
- Each part of the control block has its own cache line, because each is written at a
  different rate. A write to one part never takes away another part's line (no false
  sharing).
- Every page is touched and locked in RAM when mapped, so no page fault happens while
  publishing or reading.

### 4.6 Ring buffer

```cpp
struct alignas(128) Slot {        // one ring entry
  atomic<uint64_t> stamp;         // seq: the message's number on the bus.
                                  // 2*seq+1 while written, 2*seq+2 when
                                  // done, 0 if never written
  atomic<uint64_t> payload[7];    // 24 B header + 32 B message; atomic,
                                  // since a reader may copy mid-write
};                                // 64 B used, padded to 128 B (section 7)
```

The protocol (a seqlock per slot, with the sequence number inside the stamp):

```
// release / acquire: a reader whose acquire load sees a value stored with
// release also sees every write the writer made before that store.

WRITER  publish(message):            // never waits for a reader
  seq  = next_seq++                  // this message's number on the bus
  slot = ring[seq mod N]
  slot.stamp = 2*seq + 1             // odd: "writing seq"
  release fence                      // odd stamp is seen before any new
                                     // payload word
  slot.payload = message             // header + body, word by word
  slot.stamp = 2*seq + 2   (release) // even: "seq is complete"

READER  try_poll(seq):               // seq: the next message it wants
  slot = ring[seq mod N]
  s = slot.stamp   (acquire)
  if s < 2*seq + 2:  return NOT_YET  // not written yet, or half written
  if s > 2*seq + 2:  return LAPPED   // reused by a later message: seq lost
  copy = slot.payload
  acquire fence                      // finish the copy before the re-check
  if slot.stamp != s: return LAPPED  // writer changed it during the copy
  return OK                          // copy holds all of message seq
```

- **Sequence number in the stamp:** a plain version counter cannot tell "my message" from
  "the same slot one lap later". With the sequence number inside, lap detection is exact.
- **The two fences:** if a reader's copy saw any newer payload word, the fences make sure the
  re-check also sees the newer stamp. So a mixed (torn) copy is always rejected.
- **Acquire, not seq_cst:** they differ only in waiting for this thread's earlier release
  stores, and a reader makes none.
- **Wait-free on both sides:** a fixed number of steps, no compare-and-swap, and readers
  write nothing.

### 4.7 Snapshot table

```cpp
struct InstrumentSnapshot {     // one instrument's top of book
  uint64_t last_included_seq;   // newest ring message already in it; a
                                // reader skips deltas up to this one
  uint64_t exchange_time;       // exchange time of its last change
  uint16_t instrument_id;
  uint8_t  bid_count, ask_count;  // how many levels below are in use
  uint32_t flags;               // "bad" or "suspect": do not rebuild from it
  Level    bids[K], asks[K];    // best K levels per side, best first
};                              // 24 B + 2 x K x 8 B = 120 B when K = 6

struct alignas(128) SnapshotRecord {   // one 128 B line per instrument
  atomic<uint64_t> version;            // 0 never written, odd while
                                       // writing, even when complete
  atomic<uint64_t> words[15];          // the snapshot: 15 x 8 B = 120 B
};
```

- Written in the same order as a ring slot: odd version, fence, data, even version.
- A reader retries while the version is odd or changes, because any newer snapshot is fine.
  The retries are capped, so a writer that died mid-write cannot hang a reader.
- K = 6 is the most levels that fit in one 128 B record.

### 4.8 Publisher

```cpp
class Publisher {                 // the writer's only way onto the bus
 public:
  // Writes one ring slot; returns its seq. Used for trades. A delta
  // here fails to compile: it must go out with its snapshot.
  uint64_t publish(uint16_t instrument, const Trade& trade);

  // Writes the ring slot, then the instrument's snapshot, marked with
  // that slot's seq (last_included_seq). Returns the seq.
  template <class Message>        // a BookDelta or an InstrumentStatus
  uint64_t publish_and_update_snapshot(uint16_t instrument,
                                       const Message& message,
                                       InstrumentSnapshot snapshot);

  void heartbeat_if_due();        // store the time if 1 ms has passed
  void mark_exited();             // clean shutdown: readers see Down at once

 private:
  RingWriter ring;                // writes slots (4.6)
  SnapshotTable snapshots;        // one record per instrument (4.7)
};
```

- The message is built in registers and stored straight into the slot.
- A status message also goes out with its snapshot, so the snapshot carries the "bad" or
  "suspect" flag.

### 4.9 Consumer

```cpp
// Derived: the user's reader class (MyReader below).
template <class Derived, class WaitPolicy = SpinWait>
class Consumer {
 public:
  // Maps the bus read-only. Starts at the head hint with every
  // instrument stale (its book unknown until its snapshot is read).
  Status attach(const std::string& bus_name);
  // Tries the next message once. GotMessage: read it (on() ran, or
  // recovery skipped it). Lapped: jumped ahead. NothingNew: the wait
  // policy ran (spin, or sleep 1 ms).
  PollOnceResult poll_once();
  WriterHealth writer_health() const;     // Alive or Down (5.6)

 protected:                    // hooks: empty unless Derived defines them
  void on_lap(uint64_t lapped_seq, uint64_t resume_seq);  // all now stale
  void on_snapshot(uint16_t instrument, const InstrumentSnapshot& snap);
                               // rebuild this instrument's book from snap
  void on_stale(uint16_t instrument);   // went stale: a bad or suspect
                                        // status, or unusable snapshot
  void on_health_change(WriterHealth health);   // writer Alive <-> Down

 private:
  RingReader ring;                // reads one slot (try_poll, 4.6)
  uint64_t next_seq;              // next message to read
  std::vector<Recovery> recovery; // per instrument: stale?, and the first
                                  // seq to deliver after its snapshot
};

struct MyReader : Consumer<MyReader> {   // one on() per message type
  void on(const BookDelta& delta, const MessageInfo& info);
  void on(const Trade& trade, const MessageInfo& info);
  void on(const InstrumentStatus& status, const MessageInfo& info);
};  // info: the message's seq, instrument id and header
```

- CRTP (the reader class passes itself to its base as `Derived`): the base calls the handler
  directly, so it is inlined. A missing handler is a compile error.
- Wait policy: `SpinWait` (lowest latency, keeps a core busy) or `SleepWait` (sleeps when
  there is nothing to read).

### 4.10 Writer lock and health

- One writer per bus: an exclusive file lock (`flock`). The OS releases it when the writer
  dies, even on a kill. Taking it never waits: a second writer fails at once (5.7).
- Health comes from two words the writer keeps in the control block: its heartbeat time and
  its state (Running or Exited). Each reader judges the writer Alive or Down from them (5.6).

## 5. Problem Scenarios Handling

### 5.1 A reader falls behind (lapped)

- The writer reuses a slot before a reader has read it. The reader sees a stamp higher than
  expected: `LAPPED`.
- The reader jumps to the head hint and marks every instrument "stale".
- A newly attached reader starts the same way: at the head hint, every instrument stale.
- The writer is never told and never waits.

### 5.2 Recovery after a lap

```
on each message for instrument X (seq: its number on the bus):
  delta:
    if X is stale:                    // reader's copy of X not trusted
      snapshot = read_snapshot(X)     // retries a few times if mid-write
      if the read gave up, or the snapshot is "bad" or "suspect":
        drop the delta; X stays stale; done   // try again at next delta
      on_snapshot(X, snapshot)        // the user rebuilds X's book
      first_seq[X] = snapshot.last_included_seq + 1
      X is no longer stale
    if seq < first_seq[X]: skip       // already in the snapshot
    else: deliver it
  trade:
    deliver it                        // always: no snapshot holds trades
  status:
    if it says "bad" or "suspect": mark X stale
    deliver it                        // every status reaches on()
```

- **Why it is correct:** the writer saves X's snapshot after the slot that changed X and
  before the next slot. So a reader holding slot S finds a snapshot that already has every
  change to X before S.
- **Why lazy:** a reader pays only for the instruments it actually sees, and delivery never
  pauses for a full sweep of all snapshots.
- **Cost:** trades the lap jumped over are lost (no snapshot holds trades).

### 5.3 A torn read

The writer starts the next lap of a slot while a reader copies it. The re-check sees a new
stamp and returns `LAPPED`; the mixed copy is never delivered. There is no retry: the slot
now holds a later message.

### 5.4 The writer dies in the middle of a write

- **Mid-slot:** the stamp stays odd, so readers see `NOT_YET`. The heartbeat stops, so they
  report the writer Down.
- **Mid-snapshot:** the version stays odd. A reader gives up after its retry cap, drops that
  delta, keeps the instrument stale and tries again on the next delta.

### 5.5 The writer restarts

- A restarted writer always creates a fresh segment.
- Readers see the old writer Down. The application attaches again by name; recovery then works
  as after a lap.
- **Why not continue the old ring:** the new writer would have to find where the old one
  stopped and repair a half-written slot or snapshot while readers are still reading. A fresh
  segment avoids that and costs each reader one attach.

### 5.6 Is the writer alive?

- The writer stores a heartbeat about every millisecond, and "Exited" on a clean shutdown.
- A reader checks only when it has nothing to read; a message already proves the writer alive.

```
judge_writer (first matching rule wins; timeout T = 100 ms):
  writer state is Exited                   ->  Down   (clean exit: at once)
  this reader got a message within T       ->  Alive
  the writer's last heartbeat is within T  ->  Alive  (idle, not dead)
  otherwise                                ->  Down   (dead or paused)
```

### 5.7 A second writer

The second writer's lock attempt fails at once with "another writer running".

### 5.8 A lost packet

- The sequence check finds a gap. Every instrument is marked "suspect", and a status message
  with its snapshot goes out for each.
- Readers stop trusting suspect snapshots, so book updates stop; trades still flow.
- This is fail closed: a reader never builds a book that might be wrong.

### 5.9 Bad input to the book

- A bad add is refused and marks the instrument "bad" (section 4.3).
- Its status message goes out before the delta of the same event, so no reader applies a
  delta from a book the writer knows is wrong.

### 5.10 A reader built differently, or a buggy reader

- Different layout version or ring shape (slot count, slot size): `attach()` refuses instead
  of misreading.
- The mapping is read-only, so a buggy reader can only crash itself. Readers write nothing
  shared, so a dead reader costs the writer nothing.

## 6. Core Design Decisions

| Sub-problem | Data structure | Cost |
|---|---|---|
| Deliver every message to many readers | Ring of N slots, a seqlock per slot | O(1) publish and read; N x 128 B |
| Detect that a reader was lapped | Sequence number inside the slot stamp | O(1): one compare |
| Rebuild a book after a lap | One seqlock snapshot per instrument | O(1) per instrument, read only when needed |
| Where a new reader starts | Head hint in the control block | O(1); at most one hint interval (64) behind |
| Order id -> order | Open-addressing hash table, linear probing | O(1) expected; no allocation per order |
| Price -> level quantity | Array over a fixed price window | O(1) |
| Next best price after a level empties | Two-level bitmap + bit scan | O(1): two word scans |
| Instrument lookup | Instrument id is the array index | O(1) |
| What changed for readers | Diff of the top K levels, before and after | O(1): K is fixed |
| Message type -> handler | Fold expression + CRTP | One compare per type, inlined call |
| One writer per bus | Exclusive file lock | Only at start; nothing per message |
| Is the writer alive? | Heartbeat + state word | One load per idle check |

## 7. Optimizations

- **CRTP instead of virtual handlers.** The handler call is direct and inlined. A virtual call
  costs 9.9 more instructions per message.
- **One 64 B line per message.** Stamp + payload = exactly 64 B, so a reader moves one line
  per message. Two lines cost 42 ns more per message.
- **Poll the slot, not a global counter.** Waiting on a shared "head" counter means fetching
  two lines one after the other (counter, then slot). Polling the stamp needs one. Saves
  29.7 ns.
- **128 B slots.** Cores in different clusters share data in 128 B blocks, so two 64 B slots in
  one block could slow each other. 64 B slots were 1.75 ns cheaper per publish at full speed,
  but core placement is not controlled, so 128 B is kept.
- **Prefetch.** While applying event i, the book starts loading the order-table entry of event
  i + 4. Saves 37% when the table is much larger than the cache. This was measured on long
  batches; in the feed handler a batch is one packet, so its first 4 events get no prefetch.
- **Cold code out of line.** The writer-health check is marked cold and never inlined, so the
  polling loop stays small.
- **Testable atomics at no cost.** The ring code is written once against an "atomic policy".
  Tests swap in weakened policies to prove they catch a missing fence; the real build pays
  nothing.

## 8. Measurement

### 8.1 The clock

- Latency uses the CPU's hardware counter (`cntvct_el0`), which runs at 24 MHz: 1 tick =
  41.67 ns.
- It is the same counter in every process, so a writer's time and a reader's time can be
  subtracted directly.
- An instruction barrier (`isb`) comes before each read, so the counter is never read early.
- Percentiles are given in ticks; values under 3 ticks are shown as a range.
- The publish time is taken after the message is fully built, so encoding is not counted.
- Heartbeats and timeouts use the normal monotonic clock instead.

### 8.2 Method

| Metric | How it is measured | Comment |
|---|---|---|
| Ring hop latency | Writer stores the time just before publishing; reader takes the time after a valid copy | Excludes Consumer's own work |
| End-to-end latency | From each message's scheduled send time to the reader | A late writer counts as late, so stalls are not hidden (no coordinated omission) |
| Tail latency | Every sample in a histogram: p50, p99, p99.9, max | A mean alone hides stalls |
| Body mean | Mean of the samples at or below 667 ns | Keeps OS stalls out of the mean; the tail is still in the percentiles |
| Writer cost | Publish in large batches as fast as possible; time and instructions per publish | Messages are encoded beforehand, so only the ring is timed |
| Order book cost | Replay pre-made events through the book; time and instructions per event | No network in the loop |
| A vs B | 6 pairs of 3 s runs in the order AB, BA, AB, ...; median of pair differences | Runs are the unit: samples inside one run are not independent |
| Noise floor | Same build against itself (A/A) | An effect must be bigger than this |
| Valid run | No page faults; at least 99% of time on performance cores; full clock speed | A busy machine fails these, and the run is repeated |

- Two kinds of run: **latency runs** publish at a fixed rate (1 M msg/s); **throughput runs**
  publish as fast as possible.
- A result counts as better or worse only if all 6 pairs agree on the direction, and the
  median is larger than both the noise floor and a minimum size (1 ns, 1 tick or 3%).

### 8.3 Results

All six pairs agree in every row unless noted. Latency runs at 1 M msg/s.

| Comparison | Result | Comment |
|---|---|---|
| Ring hop, performance core to performance core | 87.8 ns body mean; p50 2 ticks; p99 250 ns | About two cache-line transfers between cores |
| Payload in one 64 B line vs two | 86.9 -> 128.9 ns | Two lines, two transfers |
| Poll a global head instead of the slot stamp | 91.3 -> 121.0 ns | Head line, then slot line, one after the other |
| Fast reader's hop, slow reader on an efficiency core: absent vs present | 86.9 -> 219.9 ns (2.5x) | Wait-free is not isolation: the slow reader pulls the slot lines away, and the writer must take them back |
| Writer per publish: alone vs one reader | 2.67 -> 26.1 ns, same instructions | Same work; the reader holds the line the writer writes next |
| 64 B slots instead of 128 B | 1.75 ns faster per publish at full speed; no difference at 1 M msg/s | 128 B kept: core placement is not controlled |
| Mutex vs ring: end-to-end p99 with a slow reader | 237 us vs 417 ns | The writer waits while a reader holds the lock: 76% of its publishes ran late. At full speed the mutex reader lost 76% of messages |
| Virtual handler instead of CRTP | +9.9 instructions per message | The indirect call blocks inlining |
| Order book: naive -> final | 147.5 -> 53.0 ns per event | The four steps below |
| - `std::map` levels -> price ladder | -29.9 ns | Tree walk -> array index |
| - `unordered_map` orders -> open addressing | -63.8 ns (5 of 6 pairs usable, so unresolved by the rule) | No heap node per order |
| - Symbol hash lookup -> id as index | -7.0 ns | No string hashing per add |
| - Prefetch, table fits in cache | -1.6 ns (-3%), too small to count | The CPU already overlaps these loads |
| Prefetch, table much larger than cache (48 MiB) | 100.0 -> 62.8 ns (-37%) | Memory misses hidden behind 4 events |

- Latency-run means move in whole-tick steps (the writer publishes right after a counter tick),
  so differences smaller than one tick are only indicative.
- Each book step is its own A/B comparison, so the steps do not add up exactly to the total.
- The benchmarked slow reader spins; a reader that sleeps when idle was not measured.

### 8.4 How correctness was validated

| What | How |
|---|---|
| Memory ordering of ring and snapshots | Mutation test on real cores: 5 broken versions (no release fence, no acquire fence, relaxed stores, relaxed loads, no re-check). Each must be caught; the real code must pass |
| Torn read, snapshot retry | Single-thread tests that force a write between the copy and the re-check |
| Recovery | Tests for every recovery branch; a reader rebuilds the writer's book across repeated laps |
| Order book | After every event, compare with a simple `std::map` model. Each optimization step must equal the naive book byte for byte |
| Wire format | Byte-exact packets; malformed packets refused |
| Full pipeline | Real processes over UDP: books match; a dropped packet fails closed |
| Crashes | Writer killed mid-slot and mid-snapshot; second writer refused |
| No allocation | A counting `operator new` around the hot paths |
| Memory errors and races | Address, undefined-behaviour and thread sanitizers |
| Rules that must not compile | `static_assert` on message rules and sizes |

### 8.5 How hypotheses were validated

- **Predictions first:** each expected result was written down before its run; 7 of 14 hit.
  The biggest miss: a slow reader was expected to cost the fast reader nothing.
- **Tests can fail:** each crash test also runs a deliberate break and checks that it is
  caught.
- **Limit of hardware testing:** on this machine, writer-side ordering mistakes never showed
  up in a 64 B slot. They are caught through the 128 B snapshot record and a test slot that
  spans two 64 B halves. A hardware test can show a fence is needed, never that it is not.

## 9. Known Limitations

- **A lost packet stops the book.** There is no gap recovery: every instrument stays
  "suspect" (5.8), so readers keep getting trades but no book updates.
- **A slow reader slows the fast ones.** The writer never waits, but a slow reader on an
  efficiency core pulls the slot lines away from it. The fast reader's hop went from 87 to
  220 ns (8.3).
- **Measurements do not control core placement.** Bad runs are only filtered out afterwards
  (8.2), and 128 B slots are kept as a guard instead of being measured per placement.

## 10. Future Scope

- **Gap recovery.** Ask the exchange to resend the missing range over TCP. Buffer new packets
  meanwhile, then replay them in order and clear "suspect".
- **Isolate slow readers.** Wait-free removes waiting, not sharing: every reader still reads
  the writer's cache lines. A relay process on a fast core copies the ring into a second
  ring, and slow readers read only that one.
- **Pin cores when measuring.** The OS used here only steers a process to performance or
  efficiency cores; it cannot fix one core or cluster. On a system that can (for example,
  Linux with isolated cores), give each process its own core, then measure 64 B against
  128 B slots for each placement.
