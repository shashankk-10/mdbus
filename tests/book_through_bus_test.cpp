// The top-K contract through the bus: FeedBook -> publish_book_output -> ring and snapshots -> a
// consumer that applies deltas (absolute quantities) and recovery snapshots.
// - Phase 1, in lock step: after every event each consumer book equals the writer's top levels,
//   no delta has more than 3 entries, and every side is sorted best first with positive qty.
// - Phase 2: the consumer is lapped again and again, and lazy recovery must still land every
//   book it touches on the writer's top levels.
// - A failure means the published deltas and snapshots do not rebuild the writer's book.

#include <cstring>
#include <iostream>
#include <vector>

#include "mdbus/book/feed_book.hpp"
#include "mdbus/book/publish_book_output.hpp"
#include "sim/order_event_generator.hpp"
#include "mdbus/bus_writer.hpp"
#include "mdbus/consumer.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

using namespace mdbus;
using namespace mdbus::book;

namespace {

using TestLayout = BusLayout<DefaultSchema, 4096>;

// Levels best first with positive quantities, then zeros.
bool is_sorted_best_first(const TopLevels& s, bool is_bid) {
  for (std::size_t k = 0; k < kTopLevelsPerSide; ++k) {
    if (k >= s.count) {
      if (s.levels[k].qty != 0) return false;
      continue;
    }
    if (s.levels[k].qty == 0) return false;
    if (k > 0 && !is_better_price(is_bid, s.levels[k - 1].price, s.levels[k].price)) return false;
  }
  return true;
}

struct BookRebuildingConsumer : Consumer<BookRebuildingConsumer, SpinWait, TestLayout> {
  std::vector<BidAskLevels> books;
  std::uint64_t trades = 0;

  explicit BookRebuildingConsumer(std::uint32_t n) : books(n) {}

  void on(const BookDelta& d, const MessageInfo& m) {
    apply_delta(books[m.instrument_id], d);
  }

  void on(const Trade&, const MessageInfo&) {
    ++trades;
  }

  void on(const InstrumentStatus&, const MessageInfo&) {}

  void on_snapshot(std::uint16_t i, const InstrumentSnapshot& s) {
    books[i] = top_levels_from_snapshot(s);
  }
};

}  // namespace

// Both phases above. Would catch a delta that is relative instead of absolute, a missing
// level change, or a snapshot that disagrees with the deltas before it.
TEST(consumer_books_reproduce_the_image) {
  GeneratorConfig config;
  config.seed = 22;
  config.instrument_count = 32;
  // Thin enough levels that a replace can empty one: the 3-entry delta.
  config.target_live_orders = 4096;
  OrderEventGenerator generator(config);
  FeedBook<> feed_book(generator.instrument_list(), 15);

  InProcessBus<TestLayout> bus(config.instrument_count);
  Publisher<TestLayout> publisher(bus.pointers());
  publisher.mark_running();

  BookRebuildingConsumer reader(config.instrument_count);
  reader.attach_in_process(bus.pointers());

  std::uint64_t bad = 0;
  std::uint64_t trades = 0;
  std::uint64_t most = 0;

  auto step = [&] {
    const BookOutput o = feed_book.apply(generator.next_event());
    if (o.trade.qty != 0) ++trades;
    most = std::max<std::uint64_t>(most, o.delta.entry_count);
    if (o.instrument_id != kNoInstrument) {
      if (!is_sorted_best_first(feed_book.top_levels(o.instrument_id, 0), true) ||
          !is_sorted_best_first(feed_book.top_levels(o.instrument_id, 1), false))
        ++bad;
    }
    if (o.has_anything_to_publish()) publish_book_output(publisher, feed_book, o, 0, 0);
  };

  // Every instrument the reader holds (not stale) must equal the writer's top levels.
  auto check_all = [&] {
    for (std::uint16_t i = 0; i < config.instrument_count; ++i) {
      if (reader.is_stale(i)) continue;
      if (std::memcmp(&reader.books[i][0], &feed_book.top_levels(i, 0), sizeof(TopLevels)) != 0)
        ++bad;
      if (std::memcmp(&reader.books[i][1], &feed_book.top_levels(i, 1), sizeof(TopLevels)) != 0)
        ++bad;
    }
  };

  for (int i = 0; i < 200'000; ++i) {
    step();
    reader.poll_until_idle();
    check_all();
  }
  CHECK(bad == 0 && reader.trades == trades && reader.stats().times_lapped == 0 && most == 3);

  // Each round publishes more slots than the ring holds before the reader drains: a lap.
  for (int round = 0; round < 20; ++round) {
    const std::uint64_t start = publisher.next_seq();
    while (publisher.next_seq() - start <= TestLayout::kSlotCount)
      step();
    reader.poll_until_idle();
    check_all();
  }

  std::cerr << "  laps " << reader.stats().times_lapped << ", resyncs "
            << reader.stats().snapshot_recoveries << '\n';
  CHECK(bad == 0 && reader.stats().times_lapped >= 20 &&
        reader.stats().snapshot_recoveries > config.instrument_count);
}

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
