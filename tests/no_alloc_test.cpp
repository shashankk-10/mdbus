// The hot paths allocate nothing.
// - How: a counting global operator new sees every allocation, and the count must not move
//   across a hot-path window.
// - Bus windows: publish (all variants), heartbeat, poll with dispatch, lap recovery, snapshot
//   recovery, and the writer-health check.
// - Feed windows, from F2 on: the book applying an event and the writer publishing its output.
//   F0 and F1 allocate a container node per new level or order, so they are left out.
// - Construction (attach, tables, ladders) is outside every window.

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

#include "sim/order_event_generator.hpp"
#include "baseline/book_ladder.hpp"
#include "mdbus/book/feed_book.hpp"
#include "mdbus/book/publish_book_output.hpp"
#include "mdbus/bus_writer.hpp"
#include "mdbus/consumer.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

namespace {
std::atomic<std::uint64_t> g_allocs{0};
}

void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t n) {
  return operator new(n);
}

// <new> declares the replaced operator delete noexcept, so these must say so too.
void operator delete(void* p) noexcept {
  std::free(p);
}

void operator delete[](void* p) noexcept {
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}

// The aligned forms, for types declared with alignas above 16 (FeedBook's instrument states).
// posix_memalign, since aligned_alloc needs a size that is a multiple of the alignment.
void* operator new(std::size_t n, std::align_val_t alignment) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  std::size_t alignment_bytes = static_cast<std::size_t>(alignment);
  if (alignment_bytes < sizeof(void*)) alignment_bytes = sizeof(void*);
  void* p = nullptr;
  if (posix_memalign(&p, alignment_bytes, n == 0 ? 1 : n) != 0) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t n, std::align_val_t alignment) {
  return operator new(n, alignment);
}

void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}

void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

using namespace mdbus;
using namespace mdbus::baseline;
using namespace mdbus::book;

namespace {
using TestLayout = BusLayout<DefaultSchema, 256>;

struct NoAllocConsumer : Consumer<NoAllocConsumer, SpinWait, TestLayout> {
  std::uint64_t n = 0;
  std::uint64_t downs = 0;  // health changes to Down

  void on(const BookDelta&, const MessageInfo&) {
    ++n;
  }

  void on(const Trade&, const MessageInfo&) {
    ++n;
  }

  void on(const InstrumentStatus&, const MessageInfo&) {
    ++n;
  }

  void on_snapshot(std::uint16_t, const InstrumentSnapshot&) {
    ++n;
  }

  void on_health_change(WriterHealth health) {
    if (health == WriterHealth::Down) ++downs;
  }
};

template <class BookConfig>
std::uint64_t feed_allocs(const std::vector<OrderEvent>& ev,
                          const std::vector<InstrumentInfo>& instrument_list, std::size_t warm) {
  FeedBook<BookConfig> feed_book(instrument_list, 15);
  InProcessBus<> bus(static_cast<std::uint32_t>(instrument_list.size()));
  Publisher<> publisher(bus.pointers());
  publisher.mark_running();

  auto run = [&](std::size_t from, std::size_t to) {
    feed_book.apply_batch(ev.data() + from, to - from, [&](std::size_t, const BookOutput& o) {
      publish_book_output(publisher, feed_book, o, 0, read_ticks());
    });
  };

  run(0, warm);
  const std::uint64_t before = g_allocs.load();
  run(warm, ev.size());
  const std::uint64_t n = g_allocs.load() - before;

  std::cerr << "  " << BookConfig::kName << ": " << n << " allocations in " << ev.size() - warm
            << " events\n";
  return n;
}
}  // namespace

// Zero allocations across every bus window listed above.
TEST(bus_hot_paths_do_not_allocate) {
  InProcessBus<TestLayout> bus(8);
  NoAllocConsumer consumer;
  consumer.attach_in_process(bus.pointers());
  // 0, so the second health check after a message reads the heartbeat (the first starts the
  // quiet clock), and ends Down: no heartbeat is that fresh.
  consumer.set_heartbeat_timeout(0);
  Publisher<TestLayout> publisher(bus.pointers());
  publisher.mark_running();

  const std::uint64_t before = g_allocs.load();
  for (int round = 0; round < 50; ++round) {
    // 600 messages per round laps the 256-slot ring, so recovery runs every round.
    for (int i = 0; i < 600; ++i) {
      const auto instrument_id = static_cast<std::uint16_t>(i % 8);
      if (i % 3 == 0) {
        Trade trade{};
        trade.price = 100;
        trade.qty = 1;
        publisher.publish_with_latency_start(instrument_id, trade, read_ticks());
      } else if (i % 3 == 1) {
        publisher.publish(instrument_id, InstrumentStatus{});
      }

      BookDelta delta{};
      delta.entry_count = 1;
      delta.entries[0] = Level{100 + i, 1};
      publisher.publish_and_update_snapshot(instrument_id, delta, InstrumentSnapshot{});
      publisher.heartbeat_if_due();
    }

    consumer.poll_until_idle();
    // Idle polls for two health checks, so the second one probes. At most one probe per 1 ms,
    // so not every round probes.
    for (std::uint32_t k = 0; k < 2 * SpinWait::kIdlePollsPerHealthCheck; ++k)
      consumer.poll_once();
  }

  CHECK(g_allocs.load() == before);
  CHECK(consumer.stats().times_lapped >= 50 && consumer.stats().snapshot_recoveries > 0 &&
        consumer.n > 0);
  CHECK(consumer.downs > 0);  // a probe ran inside the window
}

// Zero allocations while F2 and later configs apply and publish a generated stream.
TEST(feed_path_is_heap_free_from_f2) {
  GeneratorConfig config;
  config.seed = 31;
  OrderEventGenerator generator(config);
  std::vector<OrderEvent> ev(config.target_live_orders + 150'000);
  generator.fill_events(ev.data(), ev.size());
  const std::size_t warm = config.target_live_orders + 50'000;
  const std::vector<InstrumentInfo>& instrument_list = generator.instrument_list();

  CHECK(feed_allocs<F0>(ev, instrument_list, warm) > 0);  // map and hash nodes
  CHECK(feed_allocs<F1>(ev, instrument_list, warm) > 0);  // hash nodes
  CHECK(feed_allocs<F2>(ev, instrument_list, warm) == 0);
  CHECK(feed_allocs<F3>(ev, instrument_list, warm) == 0);
  CHECK(feed_allocs<F4>(ev, instrument_list, warm) == 0);

  // The flag feed_bench uses to exempt F0 and F1 from its page-fault gate says the same.
  static_assert(kAllocatesPerEvent<F0> && kAllocatesPerEvent<F1>);
  static_assert(!kAllocatesPerEvent<F2> && !kAllocatesPerEvent<F3> && !kAllocatesPerEvent<F4>);
}
