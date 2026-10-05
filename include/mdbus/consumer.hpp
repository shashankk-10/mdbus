#pragma once

// Consumer: the CRTP base every reader application derives from.
// - You write `struct MyReader : Consumer<MyReader>` with a public on(const T&, const MessageInfo&)
//   per message type in the schema, plus any hooks you want (on_lap, on_snapshot, ...).
// - Then attach("bus name"), or attach_in_process() in tests, and call poll_once() in your loop.
// - Stale: this reader's copy of an instrument's book cannot be trusted until it reads that
//   instrument's snapshot.
//
// Recovery is lazy: one instrument at a time, at its next delta. Example: 16384 slots, the
// writer has published seqs 0..505, all of them deltas for instrument 7 except one trade at 470.
// 1. Attach (or a lap): every instrument stale; reading resumes at the head hint, seq 448.
// 2. First BookDelta for 7 (seq 448): read 7's snapshot (last_included_seq 505), call
//    on_snapshot, then skip 7's deltas up to 505, already in it (57 skipped).
// 3. 7's next delta, seq 506, is delivered. The trade at 470 was delivered too: trades are
//    never filtered, because no snapshot stands in for a trade.
// - Why the snapshot is new enough: the writer updates an instrument's snapshot after the slot
//   that changed it and before the next slot. So for a seq s in hand, every earlier delta is in
//   the snapshot, and s itself is either in it (skipped) or delivered.
// - Why lazy, not eager (read every snapshot, then resume): a reader pays only for the
//   instruments it sees, when it sees them, and delivery never stops for a sweep. Eager would
//   livelock once instrument count x read time exceeded the ring's history: here 1024 x 76 ns
//   (snap_mean_ns, results/runs.csv) is about 80 us, against 16 ms at 1 M msg/s.
// - A lap also skips the trades still in the ring behind the head hint. Starting further back
//   would risk being lapped again at once.

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "mdbus/bus_reader.hpp"
#include "mdbus/clock.hpp"
#include "mdbus/ring.hpp"
#include "mdbus/snapshot_table.hpp"
#include "mdbus/writer_liveness.hpp"

namespace mdbus {

// Handed to on() with the decoded message. header and payload_words point into the reader's
// validated copy of the slot, so they are valid only during the call.
struct MessageInfo {
  std::uint64_t seq;
  std::uint16_t instrument_id;
  const MessageHeader* header;         // latency_start_ticks, publish_ticks, checksum
  const std::uint64_t* payload_words;  // the raw payload words, for verifiers
};

// What one poll_once() call did.
// - GotMessage: a valid message was read, then delivered to on() or filtered by recovery.
// - NothingNew: the next slot is not written yet; the wait policy ran.
// - Lapped: the writer overwrote the next seq to read, and reading moved to the head hint.
enum class PollOnceResult : std::uint8_t { GotMessage, NothingNew, Lapped };

// Counts since the last attach. Each says when it goes up.
struct ConsumerStats {
  std::uint64_t messages_skipped_already_in_snapshot = 0;  // a delta already in the snapshot
  std::uint64_t messages_dropped_while_stale = 0;          // a delta whose snapshot was unusable
  std::uint64_t times_lapped = 0;                          // poll_once() returned Lapped
  std::uint64_t messages_skipped_by_laps = 0;              // seqs a lap jumped over, never read
  std::uint64_t snapshot_recoveries = 0;              // on_snapshot was called
  std::uint64_t snapshot_read_retries = 0;            // seqlock retries, over all snapshot reads
  std::uint64_t snapshot_reads_gave_up = 0;           // a read hit the cap: GaveUp
  std::uint64_t messages_with_bad_instrument_id = 0;  // id >= instrument_count(); dropped
};

// What a reader does while its next slot is not ready yet: spin, or sleep 1 ms.
// - Consumer<Derived, WaitPolicy> calls idle() once per poll that found nothing, and checks the
//   writer's health every kIdlePollsPerHealthCheck such polls.
// - The writer is not involved either way: it never knows whether anyone waits.

// Spin: the lowest wake latency, and it keeps the core. The fast reader's policy (and the
// Consumer default). idle() does nothing: the next poll is the spin.
struct SpinWait {
  // An empty poll is a few ns, so this is a check every few to tens of microseconds; far below
  // the 100 ms heartbeat timeout. Not tuned.
  static constexpr std::uint32_t kIdlePollsPerHealthCheck = 4096;

  void idle() {}
};

constexpr unsigned kSleepMicroseconds = 1000;  // a message waits at most about 1 ms to be seen

// Sleep 1 ms: the slow-reader policy. Spinning holds a whole P-core, which a viewer has no use
// for.
struct SleepWait {
  static constexpr std::uint32_t kIdlePollsPerHealthCheck = 1;  // each poll already sleeps 1 ms

  void idle() {
    usleep(kSleepMicroseconds);
  }
};

// One instrument's recovery state in this reader, 16 B: the stale check and the seq filter for a
// delta read the same row.
struct InstrumentRecovery {
  bool is_stale;
  bool stale_reported;                 // on_stale already called since the last recovery
  std::uint64_t first_seq_to_deliver;  // lower seqs are already in the snapshot applied
};

// The CRTP base: Derived passes itself as the first template argument.
// - The base calls derived().on(message, info) directly, so every handler inlines. Why not
//   virtual: measured, a virtual handler costs 9.9 more instructions per message (DESIGN.md §8.3).
// - A missing on() is a compile error; one for a type Derived ignores is an empty one-liner.
// - WaitPolicy: SpinWait (the fast reader) or SleepWait (a viewer), above.
template <class Derived, class WaitPolicy = SpinWait, class Layout = BusLayout<>>
class Consumer {
 private:
  // Per-message state.
  RingReader<StdAtomics, Layout> ring_reader;
  std::uint64_t next_seq_to_read = 0;
  typename Layout::PayloadWords slot_copy{};
  // One row per instrument, indexed by id without a check: a message with a bad id is dropped
  // before its row is read.
  std::vector<InstrumentRecovery> recovery;
  ConsumerStats stats_counters;

  // Recovery and writer health.
  SegmentPointers<Layout> segment_pointers{};
  SnapshotTable<StdAtomics> snapshot_table;
  WaitPolicy wait_policy;
  WriterHealth current_writer_health = WriterHealth::Alive;
  std::uint32_t idle_polls_since_health_check = 0;
  std::uint64_t quiet_since_ns = 0;  // 0 until the first health check after a message
  std::uint64_t heartbeat_timeout_ns = kHeartbeatTimeoutNs;
  std::unique_ptr<BusReader<Layout>> named_bus;  // null for an in-process bus

 public:
  // Opens bus `name` (BusReader::open, same timeout rule) and starts reading at its head hint.
  // - Sizes the recovery table here, so polling never allocates.
  // - A failed attach leaves the current bus attached: it opens into a new BusReader first.
  // - A restarted writer makes a fresh segment: once the writer reads Down, attach again by name
  //   to follow it (mdbus_watch does).
  Status attach(const std::string& name, std::uint64_t timeout_ns = 0) {
    auto opened_bus = std::make_unique<BusReader<Layout>>();
    const Status status = opened_bus->open(name, timeout_ns);
    if (status != Status::Ok) return status;
    named_bus = std::move(opened_bus);
    stats_counters = ConsumerStats();
    start_reading(named_bus->pointers());
    set_health(WriterHealth::Alive);
    return status;
  }

  // Reads a bus in this process's own memory (tests).
  void attach_in_process(const SegmentPointers<Layout>& bus_pointers) {
    named_bus.reset();
    stats_counters = ConsumerStats();
    start_reading(bus_pointers);
    set_health(WriterHealth::Alive);
  }

  // Tries to read the next seq, once, and delivers it if there is one.
  // - Any message proves the writer alive; only an empty poll leads to a health check.
  // - A poll that finds a message makes no system call; an empty one only SleepWait's sleep.
  // Example (a fresh in-process bus, 16384 slots):
  //   nothing published               -> NothingNew
  //   the writer publishes seq 0      -> GotMessage; on() ran for seq 0, next_seq() 1
  //   the writer publishes 1..19999   -> Lapped; on_lap(1, 19968), next_seq() 19968
  //   called again                    -> GotMessage for seq 19968
  PollOnceResult poll_once() {
    const TryPollResult poll_result = ring_reader.try_poll(next_seq_to_read, slot_copy);
    if (poll_result == TryPollResult::Ok) {
      quiet_since_ns = 0;
      idle_polls_since_health_check = 0;
      set_health(WriterHealth::Alive);
      const std::uint64_t seq = next_seq_to_read;
      ++next_seq_to_read;
      decode_and_dispatch(seq);
      return PollOnceResult::GotMessage;
    }

    if (poll_result == TryPollResult::Lapped) {
      skip_ahead_after_lap();
      return PollOnceResult::Lapped;
    }
    wait_after_empty_poll();
    return PollOnceResult::NothingNew;
  }

  // Polls until nothing is ready and returns the GotMessage count, filtered ones included (58
  // in the header's run: 57 skipped deltas and the trade). For tests and quiet buses only.
  std::uint64_t poll_until_idle() {
    std::uint64_t messages_read = 0;
    while (true) {
      const PollOnceResult poll_result = poll_once();
      if (poll_result == PollOnceResult::NothingNew) return messages_read;
      if (poll_result == PollOnceResult::GotMessage) ++messages_read;
    }
  }

  // instrument_id < instrument_count().
  bool is_stale(std::uint16_t instrument_id) const {
    return recovery[instrument_id].is_stale;
  }

  std::uint64_t next_seq() const {
    return next_seq_to_read;
  }

  const ConsumerStats& stats() const {
    return stats_counters;
  }

  WriterHealth writer_health() const {
    return current_writer_health;
  }

  const SegmentPointers<Layout>& pointers() const {
    return segment_pointers;
  }

  std::uint32_t instrument_count() const {
    return segment_pointers.instrument_count;
  }

  void set_heartbeat_timeout(std::uint64_t timeout_ns) {
    heartbeat_timeout_ns = timeout_ns;
  }

  // A copy of instrument_id's current snapshot, outside the message stream (a viewer's top of
  // book, a test's final check). instrument_id < instrument_count().
  SnapshotReadResult read_snapshot(std::uint16_t instrument_id,
                                   InstrumentSnapshot& snapshot) const {
    return snapshot_table.read(instrument_id, snapshot);
  }

 protected:
  // Hooks, empty by default. Derived replaces one by declaring a public member of the same name.
  // A misspelled hook compiles and is never called: the default runs instead.
  // - on_lap: reading moved from lapped_seq to resume_seq; every instrument is now stale.
  // - on_snapshot: the instrument recovered from this snapshot (step 2 of recovery) and is no
  //   longer stale. A never-written snapshot arrives as an empty book.
  // - on_stale: the instrument went stale for a reason other than a lap or an attach (a bad or
  //   suspect status message, a snapshot it could not use). At most once until it recovers.
  // - on_health_change: the writer's health changed (Alive or Down).
  void on_lap(std::uint64_t /*lapped_seq*/, std::uint64_t /*resume_seq*/) {}
  void on_snapshot(std::uint16_t /*instrument_id*/, const InstrumentSnapshot& /*snapshot*/) {}
  void on_stale(std::uint16_t /*instrument_id*/) {}
  void on_health_change(WriterHealth /*health*/) {}

 private:
  Derived& derived() {
    return static_cast<Derived&>(*this);
  }

  // Points every part at a bus and starts at its head hint with every instrument stale: a new
  // reader is one that lapped at attach time, without the on_lap call.
  void start_reading(const SegmentPointers<Layout>& bus_pointers) {
    segment_pointers = bus_pointers;
    ring_reader = RingReader<StdAtomics, Layout>(bus_pointers.slots, bus_pointers.control);
    snapshot_table = SnapshotTable<StdAtomics>(bus_pointers.snapshot_records);

    recovery.assign(bus_pointers.instrument_count, InstrumentRecovery{true, false, 0});
    idle_polls_since_health_check = 0;
    quiet_since_ns = 0;

    next_seq_to_read = ring_reader.head_hint();
  }

  // Delivering one message.

  // Checks the instrument id, then calls filter_and_deliver for the message's type.
  void decode_and_dispatch(std::uint64_t seq) {
    Payload<Layout::kPayloadWords> payload;
    std::memcpy(&payload, slot_copy.data(), sizeof payload);
    if (payload.header.instrument_id >= segment_pointers.instrument_count) {
      ++stats_counters.messages_with_bad_instrument_id;
      return;
    }
    const MessageInfo info{seq, payload.header.instrument_id, &payload.header, slot_copy.data()};
    // A type id outside the schema reaches no handler: only a corrupt slot, or a writer whose
    // schema changed without a kLayoutVersion bump, could hold one.
    Layout::Schema::dispatch(payload.header.type_id, payload.body,
                             [&](const auto& message) { filter_and_deliver(message, info); });
  }

  // The recovery filter, then on().
  // - InstrumentStatus with a bad or suspect flag: mark the instrument stale, report it once.
  // - BookDelta: a stale instrument recovers first (or the delta is dropped); then a delta
  //   already in the snapshot is skipped.
  // - Trade and everything else: always delivered.
  template <class Message>
  void filter_and_deliver(const Message& message, const MessageInfo& info) {
    if constexpr (std::is_same_v<Message, InstrumentStatus>) {
      if ((message.instrument_flags & kSnapshotUnusableFlags) != 0) {
        recovery[info.instrument_id].is_stale = true;
        report_stale(info.instrument_id);
      }
    }

    if constexpr (kIncludedInSnapshot<Message>) {
      if (recovery[info.instrument_id].is_stale) {
        const bool recovered = recover_from_snapshot(info.instrument_id);
        if (!recovered) {
          ++stats_counters.messages_dropped_while_stale;
          return;
        }
      }
      if (info.seq < recovery[info.instrument_id].first_seq_to_deliver) {
        ++stats_counters.messages_skipped_already_in_snapshot;
        return;
      }
    }

    derived().on(message, info);
  }

  // Recovery.

  // Step 2 of recovery: read instrument_id's snapshot and, if usable, hand it to on_snapshot.
  // - Ok: deliver from last_included_seq + 1.
  // - NeverWritten: no delta for it came before this slot, so an empty book is exact.
  // - GaveUp, or a snapshot flagged bad or suspect: stays stale, false (the delta is dropped).
  bool recover_from_snapshot(std::uint16_t instrument_id) {
    InstrumentSnapshot snapshot;
    const SnapshotReadResult read_result = snapshot_table.read(instrument_id, snapshot);
    stats_counters.snapshot_read_retries += read_result.retries;
    if (read_result.status == SnapshotReadStatus::GaveUp) {
      ++stats_counters.snapshot_reads_gave_up;
      report_stale(instrument_id);
      return false;
    }
    std::uint64_t first_seq_to_deliver = 0;
    if (read_result.status == SnapshotReadStatus::NeverWritten) {
      snapshot = InstrumentSnapshot{};
      snapshot.instrument_id = instrument_id;
    } else {
      if ((snapshot.instrument_flags & kSnapshotUnusableFlags) != 0) {
        report_stale(instrument_id);
        return false;
      }
      first_seq_to_deliver = snapshot.last_included_seq + 1;
    }

    derived().on_snapshot(instrument_id, snapshot);
    // Not stale any more, on_stale re-armed, and deltas below first_seq_to_deliver skipped as
    // already in the snapshot.
    recovery[instrument_id] = InstrumentRecovery{false, false, first_seq_to_deliver};
    ++stats_counters.snapshot_recoveries;
    return true;
  }

  // Step 1 of recovery after a lap: jump to the head hint (never back onto the lapped slot) and
  // mark every instrument stale.
  void skip_ahead_after_lap() {
    const std::uint64_t lapped_seq = next_seq_to_read;
    next_seq_to_read = std::max(ring_reader.head_hint(), lapped_seq + 1);
    for (InstrumentRecovery& row : recovery)
      row.is_stale = true;
    ++stats_counters.times_lapped;
    stats_counters.messages_skipped_by_laps += next_seq_to_read - lapped_seq;
    derived().on_lap(lapped_seq, next_seq_to_read);
  }

  // Calls on_stale only the first time since the instrument last recovered, so a long stale
  // spell produces one callback, not one per message.
  void report_stale(std::uint16_t instrument_id) {
    InstrumentRecovery& row = recovery[instrument_id];
    if (row.stale_reported) return;
    row.stale_reported = true;
    derived().on_stale(instrument_id);
  }

  // Writer health.

  // After an empty poll: the health check when it is due, then the wait policy's pause.
  void wait_after_empty_poll() {
    ++idle_polls_since_health_check;
    if (idle_polls_since_health_check >= WaitPolicy::kIdlePollsPerHealthCheck) {
      idle_polls_since_health_check = 0;
      check_writer_health();
    }
    wait_policy.idle();
  }

  // Updates the writer's health (judge_writer). The first check after a message only starts the
  // quiet clock: a message proves the writer alive.
  // - Never inlined (and cold), so the liveness code stays out of poll_once(): inlined, it made
  //   the spinning poll_once save two more registers on every poll (123 instructions, 97 now).
  [[gnu::noinline, gnu::cold]] void check_writer_health() {
    const std::uint64_t now_ns = steady_clock_ns();
    if (quiet_since_ns == 0) quiet_since_ns = now_ns;
    set_health(judge_writer(*segment_pointers.control, now_ns, now_ns - quiet_since_ns,
                            heartbeat_timeout_ns));
  }

  void set_health(WriterHealth health) {
    if (health == current_writer_health) return;
    current_writer_health = health;
    derived().on_health_change(health);
  }
};

}  // namespace mdbus
