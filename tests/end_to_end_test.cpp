// End to end over real processes: mdbus_exchange_sim multicasts the generator's stream on
// loopback, mdbus_feed_handler keeps the book and writes the bus, and a consumer thread here
// applies every delta.
// - Clean run: every instrument ends fresh, with the consumer's book equal to the writer's
//   snapshot.
// - Induced drop: the feed raises kInstrumentSuspect, every instrument goes stale and stays
//   stale, every snapshot carries the bit (fail closed), and trades still arrive.
// - Both runs: latency_start_ticks (the feed's receive time) <= publish_ticks <= this reader's
//   clock, so the timestamps hold across processes.
// - The printed receive-to-handler latency is an indication only: loopback, a sender that sleeps
//   between packets, and no measurement gates.

#include <algorithm>
#include <array>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include "mdbus/consumer.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

using namespace mdbus;

namespace {

constexpr std::uint16_t kInstrumentCount = 16;

struct TopOfBookConsumer : Consumer<TopOfBookConsumer> {
  std::array<BidAskLevels, kInstrumentCount> books{};
  std::uint64_t trades = 0;
  std::uint64_t messages_with_latency_start = 0;
  std::uint64_t disorder = 0;
  std::uint64_t stale_calls = 0;
  std::vector<std::uint64_t> latency;  // ticks, feed receive to handler

  void on(const BookDelta& d, const MessageInfo& m) {
    apply_delta(books[m.instrument_id], d);
    check_latency_ticks(m);
  }

  void on(const Trade&, const MessageInfo& m) {
    ++trades;
    check_latency_ticks(m);
  }

  void on(const InstrumentStatus&, const MessageInfo&) {}

  void on_snapshot(std::uint16_t instrument_id, const InstrumentSnapshot& snapshot) {
    books[instrument_id] = top_levels_from_snapshot(snapshot);
  }

  void on_stale(std::uint16_t) {
    ++stale_calls;
  }

  void check_latency_ticks(const MessageInfo& m) {
    if (m.header->latency_start_ticks == 0) return;
    const std::uint64_t now = read_ticks();
    ++messages_with_latency_start;
    if (m.header->latency_start_ticks > m.header->publish_ticks || m.header->publish_ticks > now)
      ++disorder;
    if (latency.size() < latency.capacity()) latency.push_back(now - m.header->latency_start_ticks);
  }
};

double ticks_to_microseconds(std::uint64_t t) {
  return static_cast<double>(t) * kNsPerTick / 1e3;
}

// Runs both processes to completion with a reader attached, then checks every instrument.
void run_scenario(unsigned run_index, bool drop_packets) {
  const std::string bus = mdbus_test::make_test_bus_name("u" + std::to_string(run_index));

  // A group from the pid, so parallel test runs never hear each other, and a port per run.
  const auto pid = static_cast<unsigned>(getpid());
  const std::string group =
      "239.255." + std::to_string((pid >> 8) & 255) + "." + std::to_string(pid & 255);
  const std::string port = std::to_string(20000 + pid % 10000 + run_index);

  // The generator opens with 16,384 adds (its default live-order target), so 40,000 events
  // leave room for executions and cancels.
  const std::vector<std::string> common = {"--group", group, "--port", port,
                                           "--seed", "7", "--instruments", "16"};
  std::vector<std::string> feed_args = {MDBUS_FEED_HANDLER_PATH, "--bus", bus, "--max-ms", "20000"};
  std::vector<std::string> sim_args = {MDBUS_EXCHANGE_SIM_PATH, "--events", "40000", "--rate",
                                       "20000"};
  if (drop_packets) sim_args.insert(sim_args.end(), {"--drop-at", "300", "--drop-count", "2"});
  feed_args.insert(feed_args.end(), common.begin(), common.end());
  sim_args.insert(sim_args.end(), common.begin(), common.end());

  mdbus_test::ChildProcess feed(mdbus_test::spawn_program(feed_args));
  REQUIRE(feed.pid > 0);

  TopOfBookConsumer reader;
  reader.latency.reserve(1 << 16);

  // The feed opens its socket (bound, group joined) before it creates the bus, so attaching
  // proves it is listening.
  REQUIRE(reader.attach(bus, 5'000'000'000) == Status::Ok);

  // An O_RDONLY object caps the mapping's maximum protection: not even mprotect makes a reader's
  // view writable, so a buggy reader faults instead of corrupting the bus.
  CHECK(mprotect(reader.pointers().control, kPageSize, PROT_READ | PROT_WRITE) != 0);

  std::atomic<bool> stop{false};
  std::thread poller([&] {
    while (!stop.load(std::memory_order_relaxed)) reader.poll_once();
  });

  mdbus_test::ChildProcess sim(mdbus_test::spawn_program(sim_args));
  const int sim_rc = sim.wait_for_exit_code();
  const int feed_rc = feed.wait_for_exit_code();  // the feed leaves at the end of stream
  stop.store(true);
  poller.join();
  reader.poll_until_idle();

  std::cerr << "  trades " << reader.trades << ", messages_with_latency_start "
            << reader.messages_with_latency_start << ", stale calls " << reader.stale_calls << "\n";
  if (!reader.latency.empty()) {
    std::sort(reader.latency.begin(), reader.latency.end());
    const std::size_t last = reader.latency.size() - 1;
    std::cerr << std::fixed << std::setprecision(1)
              << "  feed receive to consumer handler (indicative): p50 "
              << ticks_to_microseconds(reader.latency[last / 2]) << " us, p99 "
              << ticks_to_microseconds(reader.latency[last * 99 / 100]) << " us, n "
              << reader.latency.size() << "\n";
  }

  CHECK(sim_rc == 0);
  CHECK(feed_rc == 0);
  CHECK(reader.trades > 0);
  CHECK(reader.messages_with_latency_start > 0);
  CHECK(reader.disorder == 0);
  CHECK(reader.stale_calls >= (drop_packets ? kInstrumentCount : 0));

  for (std::uint16_t i = 0; i < kInstrumentCount; ++i) {
    InstrumentSnapshot s;
    CHECK(reader.read_snapshot(i, s).status == SnapshotReadStatus::Ok);
    CHECK(s.instrument_flags == (drop_packets ? kInstrumentSuspect : 0));
    CHECK(reader.is_stale(i) == drop_packets);
    const BidAskLevels b = top_levels_from_snapshot(s);
    if (!drop_packets) CHECK(std::memcmp(&b, &reader.books[i], sizeof b) == 0);
  }
}

}  // namespace

// No loss: the consumer's books end equal to the writer's snapshots.
TEST(clean_stream_books_match) {
  run_scenario(1, false);
}

// A dropped packet must mark every instrument suspect and keep it so. Would catch a feed that
// carries on after a gap, publishing books it can no longer vouch for.
TEST(dropped_packets_raise_suspect) {
  run_scenario(2, true);
}

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
