// Consumer on one process: recovery, and the writer-health verdicts that need no second process.
// - Recovery: in order, after a lap, with a suspect instrument, and each way recover_from_snapshot
//   can find the snapshot (older than the delta, never written, mid-write, flagged).
// - How: every delta for an instrument carries its running count as qty, and every snapshot
//   carries the count it includes, so a delta applied twice, skipped or wrongly filtered shows
//   as a gap in the count.
// - Health: a writer that exits reads Down at the next health check; one that goes silent reads
//   Down after the timeout, and Alive again at its next message.
// - A failure means recovery can leave a consumer's book silently wrong, or a reader misjudges
//   its writer.

#include "mdbus/consumer.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "mdbus/bus_writer.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

using namespace mdbus;

namespace {
using TestLayout = BusLayout<DefaultSchema, 128>;
constexpr std::uint32_t kInstrumentCount = 4;

struct DeltaCountingConsumer : Consumer<DeltaCountingConsumer, SpinWait, TestLayout> {
  std::uint32_t cnt[kInstrumentCount] = {};
  std::uint64_t trades = 0;
  std::uint64_t errors = 0;
  std::uint64_t laps = 0;
  std::uint64_t lap_to = 0;
  std::uint64_t stale_calls = 0;
  std::uint64_t downs = 0;
  InstrumentSnapshot last_snapshot{};  // the last one on_snapshot was handed

  void on(const BookDelta& d, const MessageInfo& m) {
    if (d.entries[0].qty != cnt[m.instrument_id] + 1) ++errors;
    cnt[m.instrument_id] = d.entries[0].qty;
  }

  void on(const Trade&, const MessageInfo&) {
    ++trades;
  }

  void on(const InstrumentStatus&, const MessageInfo&) {}

  void on_snapshot(std::uint16_t instrument_id, const InstrumentSnapshot& s) {
    cnt[instrument_id] = s.bids[0].qty;
    last_snapshot = s;
  }

  void on_lap(std::uint64_t, std::uint64_t to) {
    ++laps;
    lap_to = to;
  }

  void on_stale(std::uint16_t) {
    ++stale_calls;
  }

  void on_health_change(WriterHealth health) {
    if (health == WriterHealth::Down) ++downs;
  }

  // Empty polls until one health check has run.
  void poll_until_health_checked() {
    for (std::uint32_t i = 0; i < SpinWait::kIdlePollsPerHealthCheck; ++i)
      poll_once();
  }
};

// A bus, a consumer attached at the start, and a writer with its own counts.
struct ConsumerFixture {
  InProcessBus<TestLayout> bus{kInstrumentCount};
  DeltaCountingConsumer consumer;
  Publisher<TestLayout> publisher{bus.pointers()};
  std::uint32_t cnt[kInstrumentCount] = {};
  std::vector<std::uint64_t> trade_seqs;

  ConsumerFixture() {
    consumer.attach_in_process(bus.pointers());
    publisher.mark_running();
  }

  void delta(std::uint16_t instrument_id, std::uint32_t instrument_flags = 0) {
    publisher.publish_and_update_snapshot(instrument_id, next_delta(instrument_id),
                                          snapshot_of_count(instrument_id, instrument_flags));
  }

  // A delta's slot without its snapshot, as a reader sees it between the two writes of
  // publish_and_update_snapshot. Returns its seq.
  std::uint64_t delta_slot_only(std::uint16_t instrument_id) {
    const std::uint64_t seq = publisher.next_seq();
    publisher.publish_encoded_words(
        Publisher<TestLayout>::encode_payload(seq, instrument_id, next_delta(instrument_id), 0));
    return seq;
  }

  // SnapshotTable::write's stores, split so a test can poll in between: the odd version, then
  // the snapshot of seq (the writer's count) and the even version.
  void start_snapshot_write(std::uint16_t instrument_id) {
    SnapshotRecord<StdAtomics>& record = bus.pointers().snapshot_records[instrument_id];
    StdAtomics::store_relaxed(record.version, StdAtomics::load_relaxed(record.version) + 1);
  }

  void finish_snapshot_write(std::uint16_t instrument_id, std::uint64_t seq) {
    InstrumentSnapshot snapshot = snapshot_of_count(instrument_id, 0);
    snapshot.last_included_seq = seq;
    snapshot.instrument_id = instrument_id;
    std::uint64_t words[kSnapshotWords];
    std::memcpy(words, &snapshot, sizeof words);
    SnapshotRecord<StdAtomics>& record = bus.pointers().snapshot_records[instrument_id];
    for (std::size_t i = 0; i < kSnapshotWords; ++i)
      StdAtomics::store_relaxed(record.snapshot_words[i], words[i]);
    StdAtomics::store_release(record.version, StdAtomics::load_relaxed(record.version) + 1);
  }

  void trade(std::uint16_t instrument_id) {
    trade_seqs.push_back(publisher.publish(instrument_id, Trade{100, 1, 0, {}}));
  }

  // Trades until the head hint is the next seq, so a consumer that attaches then starts at the
  // next message and has seen none before it.
  void trades_until_head_hint_is_next_seq() {
    while (publisher.next_seq() % RingWriter<StdAtomics, TestLayout>::kPublishesPerHeadHint != 0)
      trade(3);
  }

 private:
  BookDelta next_delta(std::uint16_t instrument_id) {
    ++cnt[instrument_id];
    BookDelta d{};
    d.side = 0;
    d.entry_count = 1;
    d.entries[0] = Level{100, cnt[instrument_id]};
    return d;
  }

  InstrumentSnapshot snapshot_of_count(std::uint16_t instrument_id,
                                       std::uint32_t instrument_flags) const {
    InstrumentSnapshot snapshot{};
    snapshot.bid_count = 1;
    snapshot.bids[0] = Level{100, cnt[instrument_id]};
    snapshot.instrument_flags = instrument_flags;
    return snapshot;
  }
};
}  // namespace

// Recovery.

// Read in order: each instrument recovers once from its snapshot, then no count gaps, no laps
// and every trade seen.
TEST(in_order) {
  ConsumerFixture fixture;
  for (std::uint32_t i = 0; i < 400; ++i) {
    fixture.delta(static_cast<std::uint16_t>(i % kInstrumentCount));
    if (i % 3 == 0) fixture.trade(1);
    fixture.consumer.poll_until_idle();
  }

  CHECK(fixture.consumer.errors == 0 && fixture.consumer.laps == 0 &&
        fixture.consumer.trades == fixture.trade_seqs.size());
  for (std::uint16_t i = 0; i < kInstrumentCount; ++i)
    CHECK(fixture.consumer.cnt[i] == fixture.cnt[i] && !fixture.consumer.is_stale(i));
  // Each instrument recovered once, from a snapshot that already held the delta that asked.
  CHECK(fixture.consumer.stats().snapshot_recoveries == kInstrumentCount &&
        fixture.consumer.stats().messages_skipped_already_in_snapshot == kInstrumentCount);
}

// A consumer lapped mid-stream recovers from snapshots and its counts carry on with no gap.
// Would catch a replayed or dropped delta around the snapshot boundary.
TEST(lap) {
  ConsumerFixture fixture;
  for (std::uint32_t i = 0; i < 1000; ++i) {
    if (i % 2 == 1)
      fixture.delta(static_cast<std::uint16_t>(i % kInstrumentCount));
    else
      fixture.trade(0);
  }

  fixture.consumer.poll_until_idle();
  REQUIRE(fixture.consumer.laps == 1);
  CHECK(fixture.consumer.errors == 0 &&
        fixture.consumer.stats().messages_skipped_by_laps == fixture.consumer.lap_to);
  for (std::uint32_t i = 0; i < kInstrumentCount; ++i)
    CHECK(fixture.consumer.cnt[i] == fixture.cnt[i]);

  // Trades past the resume point are all delivered, even those a snapshot's last_included_seq
  // covers.
  std::uint64_t expect = 0;
  for (std::uint64_t s : fixture.trade_seqs) {
    if (s >= fixture.consumer.lap_to) ++expect;
  }
  CHECK(fixture.consumer.trades == expect &&
        fixture.consumer.stats().messages_skipped_already_in_snapshot > 0);
}

// kInstrumentSuspect fails closed: the instrument goes stale, flagged snapshots are refused, so its
// deltas stop applying, and trades still arrive. An instrument past the table is counted.
TEST(suspect_fails_closed) {
  ConsumerFixture fixture;
  fixture.delta(2);
  fixture.consumer.poll_until_idle();
  REQUIRE(!fixture.consumer.is_stale(2));

  InstrumentSnapshot flagged{};
  flagged.instrument_flags = kInstrumentSuspect;
  fixture.publisher.publish_and_update_snapshot(2, InstrumentStatus{kInstrumentSuspect, {}, 0},
                                                flagged);
  fixture.delta(2, kInstrumentSuspect);
  fixture.trade(2);
  fixture.consumer.poll_until_idle();
  CHECK(fixture.consumer.is_stale(2) &&
        fixture.consumer.stats().messages_dropped_while_stale == 1 &&
        fixture.consumer.stale_calls == 1);
  CHECK(fixture.consumer.trades == 1 && fixture.consumer.cnt[2] == 1);

  fixture.trade(static_cast<std::uint16_t>(kInstrumentCount));
  fixture.consumer.poll_until_idle();
  CHECK(fixture.consumer.stats().messages_with_bad_instrument_id == 1 &&
        fixture.consumer.trades == 1);
}

// A reader that attaches between a delta's slot and its snapshot reads a snapshot older than the
// delta in hand, so the delta is delivered on top of it.
// Would catch a recovery that skips the one delta its snapshot does not hold yet.
TEST(older_snapshot_delivers_the_delta) {
  ConsumerFixture fixture;
  fixture.delta(1);  // seq 0, with its snapshot: count 1
  fixture.trades_until_head_hint_is_next_seq();
  const std::uint64_t seq = fixture.delta_slot_only(1);  // count 2; the snapshot still holds 1
  DeltaCountingConsumer late;
  late.attach_in_process(fixture.bus.pointers());
  REQUIRE(late.next_seq() == seq);

  late.poll_until_idle();
  CHECK(late.stats().snapshot_recoveries == 1 && late.last_snapshot.last_included_seq == 0);
  CHECK(late.cnt[1] == 2 && late.errors == 0 && !late.is_stale(1) &&
        late.stats().messages_skipped_already_in_snapshot == 0);
}

// An instrument's first delta read before its first snapshot write: NeverWritten, which is an
// exact empty book, so the delta is delivered on top of it.
// Would catch a NeverWritten taken as a snapshot that already holds the delta.
TEST(never_written_snapshot_is_an_empty_book) {
  ConsumerFixture fixture;
  fixture.delta_slot_only(0);  // count 1; record 0 has version 0
  fixture.consumer.poll_until_idle();
  const ConsumerStats& stats = fixture.consumer.stats();
  CHECK(stats.snapshot_recoveries == 1 && fixture.consumer.last_snapshot.bid_count == 0);
  CHECK(fixture.consumer.cnt[0] == 1 && fixture.consumer.errors == 0 &&
        !fixture.consumer.is_stale(0) && stats.messages_skipped_already_in_snapshot == 0);
}

// The snapshot is mid-write (odd version) for longer than the read's retry budget: GaveUp, so the
// delta is dropped and on_stale fires once. Once the write finishes, the next delta recovers and
// the count is whole again, the dropped delta included.
// Would catch a GaveUp that recovers anyway (from an empty or torn book) or stays silent.
TEST(snapshot_mid_write_drops_the_delta) {
  ConsumerFixture fixture;
  const std::uint64_t seq = fixture.delta_slot_only(2);  // count 1
  fixture.start_snapshot_write(2);
  fixture.consumer.poll_until_idle();
  const ConsumerStats& stats = fixture.consumer.stats();
  CHECK(fixture.consumer.is_stale(2) && fixture.consumer.stale_calls == 1);
  CHECK(stats.snapshot_reads_gave_up == 1 &&
        stats.snapshot_read_retries == kSnapshotReadMaxRetries);
  CHECK(stats.messages_dropped_while_stale == 1 && stats.snapshot_recoveries == 0);

  fixture.finish_snapshot_write(2, seq);
  fixture.delta(2);  // count 2
  fixture.consumer.poll_until_idle();
  CHECK(!fixture.consumer.is_stale(2) && fixture.consumer.cnt[2] == 2 &&
        fixture.consumer.errors == 0 && stats.snapshot_recoveries == 1);
}

// A reader that never saw the status marking an instrument bad (it attached after it) still
// refuses the flagged snapshot, and reports the instrument stale itself.
// Would catch a flagged snapshot accepted, or refused without on_stale.
TEST(flagged_snapshot_without_its_status) {
  ConsumerFixture fixture;
  InstrumentSnapshot flagged{};
  flagged.instrument_flags = kInstrumentBad;
  fixture.publisher.publish_and_update_snapshot(0, InstrumentStatus{kInstrumentBad, {}, 0},
                                                flagged);
  fixture.trades_until_head_hint_is_next_seq();
  fixture.delta(0, kInstrumentBad);
  DeltaCountingConsumer late;
  late.attach_in_process(fixture.bus.pointers());

  late.poll_until_idle();
  CHECK(late.is_stale(0) && late.stale_calls == 1 && late.cnt[0] == 0);
  CHECK(late.stats().messages_dropped_while_stale == 1 && late.stats().snapshot_recoveries == 0);
}

// Writer health.

// A named bus whose writer closes (WriterState Exited): the reader reports Down at its next
// health check, not after the 100 ms timeout. The restarted writer makes a fresh segment, which
// the reader follows by attaching again by name.
// Would catch the state word read only once the reader has been quiet that long.
TEST(exited_writer_is_down_at_once) {
  const std::string bus = mdbus_test::make_test_bus_name("cr");
  BusWriter<TestLayout> first;
  REQUIRE(first.open(bus, kInstrumentCount) == Status::Ok);
  DeltaCountingConsumer consumer;
  REQUIRE(consumer.attach(bus) == Status::Ok);
  first.publisher().publish(0, Trade{100, 1, 0, {}});
  consumer.poll_until_idle();
  REQUIRE(consumer.trades == 1);

  first.close();
  consumer.poll_until_health_checked();
  CHECK(consumer.writer_health() == WriterHealth::Down && consumer.downs == 1);

  BusWriter<TestLayout> second;
  REQUIRE(second.open(bus, kInstrumentCount) == Status::Ok);
  REQUIRE(consumer.attach(bus) == Status::Ok);
  CHECK(consumer.writer_health() == WriterHealth::Alive);
  second.publisher().publish(0, Trade{100, 1, 0, {}});
  consumer.poll_until_idle();
  CHECK(consumer.trades == 2);
}

// A writer that goes quiet: Alive while it keeps its heartbeat, Down once the reader has seen
// neither a message nor a heartbeat for the timeout (a dead or stopped writer), and Alive again
// at its next message.
// Would catch a quiet but heartbeating writer called Down, or a silent one never called Down.
TEST(silent_writer_is_down_after_the_timeout) {
  ConsumerFixture fixture;
  constexpr unsigned kTimeoutMs = 20;
  fixture.consumer.set_heartbeat_timeout(kTimeoutMs * kNsPerMillisecond);
  fixture.trade(0);
  fixture.consumer.poll_until_idle();

  const std::uint64_t quiet_until_ns = steady_clock_ns() + 2 * kTimeoutMs * kNsPerMillisecond;
  while (steady_clock_ns() < quiet_until_ns) {
    fixture.publisher.heartbeat_if_due();
    fixture.consumer.poll_until_health_checked();
  }
  CHECK(fixture.consumer.writer_health() == WriterHealth::Alive);

  mdbus_test::sleep_ms(2 * kTimeoutMs);  // no heartbeat now
  fixture.consumer.poll_until_health_checked();
  CHECK(fixture.consumer.writer_health() == WriterHealth::Down && fixture.consumer.downs == 1);

  fixture.trade(0);
  fixture.consumer.poll_until_idle();
  CHECK(fixture.consumer.writer_health() == WriterHealth::Alive && fixture.consumer.trades == 2);
}
