// Consumer recovery on one process: in order, after a lap, and with a suspect instrument.
// - How: every delta for an instrument carries its running count as qty, and every snapshot
//   carries the count it includes, so a delta applied twice, skipped or wrongly filtered shows
//   as a gap in the count.
// - A failure means recovery can leave a consumer's book silently wrong.

#include <vector>

#include "mdbus/bus_writer.hpp"
#include "mdbus/consumer.hpp"
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
  }

  void on_lap(std::uint64_t, std::uint64_t to) {
    ++laps;
    lap_to = to;
  }

  void on_stale(std::uint16_t) {
    ++stale_calls;
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
    ++cnt[instrument_id];
    BookDelta d{};
    d.side = 0;
    d.entry_count = 1;
    d.entries[0] = Level{100, cnt[instrument_id]};
    InstrumentSnapshot snapshot{};
    snapshot.bid_count = 1;
    snapshot.bids[0] = d.entries[0];
    snapshot.instrument_flags = instrument_flags;
    publisher.publish_and_update_snapshot(instrument_id, d, snapshot);
  }

  void trade(std::uint16_t instrument_id) {
    trade_seqs.push_back(publisher.publish(instrument_id, Trade{100, 1, 0, {}}));
  }
};
}  // namespace

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

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
