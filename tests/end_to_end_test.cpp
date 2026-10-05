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
// - The programs at their edges: SIGTERM ends a feed handler's run cleanly; --max-ms and
//   --drop-count at the top of their range do not wrap; a feed handler busy with packets that
//   publish nothing keeps its heartbeat fresh, and counts duplicates and an end of stream that
//   carries a message; mdbus_watch --destroy refuses while a writer holds the bus.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "mdbus/bus_reader.hpp"
#include "mdbus/bus_writer.hpp"
#include "mdbus/consumer.hpp"
#include "mdbus/feed/multicast_socket.hpp"
#include "mdbus/feed/wire_format.hpp"
#include "sim/order_event_generator.hpp"
#include "src/command_line.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

using namespace mdbus;

namespace {

constexpr std::uint16_t kInstrumentCount = 16;
constexpr std::uint64_t kStartTimeoutNs = 5'000'000'000;

// A multicast group from the pid, so parallel test runs never hear each other, and a port of its
// own per test.
struct FeedLink {
  std::string group;
  std::string port;
  sockaddr_in address{};
};

FeedLink feed_link(unsigned port_offset) {
  const auto pid = static_cast<unsigned>(getpid());
  FeedLink link;
  link.group = "239.255." + std::to_string((pid >> 8) & 255) + "." + std::to_string(pid & 255);
  const auto port = static_cast<std::uint16_t>(20000 + pid % 10000 + port_offset);
  link.port = std::to_string(port);
  make_multicast_address(link.group, port, link.address);
  return link;
}

// A feed handler on bus, listening on link, with 4 instruments of the default seed.
std::vector<std::string> feed_handler_args(const std::string& bus, const FeedLink& link) {
  std::vector<std::string> args = {MDBUS_FEED_HANDLER_PATH, "--bus", bus};
  args.insert(args.end(), {"--group", link.group, "--port", link.port, "--instruments", "4"});
  return args;
}

std::string file_text(const std::string& path) {
  std::ifstream file(path);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

int run_watch_destroy(const std::string& bus) {
  mdbus_test::ChildProcess watch(
      mdbus_test::spawn_program({MDBUS_WATCH_PATH, "--bus", bus, "--destroy"}));
  return watch.wait_for_exit_code();
}

// Spins until message seq is in the ring; false if it is not there within kStartTimeoutNs.
bool wait_until_published(const RingReader<StdAtomics, BusLayout<>>& ring, std::uint64_t seq) {
  BusLayout<>::PayloadWords words;
  const std::uint64_t deadline = steady_clock_ns() + kStartTimeoutNs;
  while (ring.try_poll(seq, words) != TryPollResult::Ok) {
    if (steady_clock_ns() > deadline) return false;
  }
  return true;
}

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
  const FeedLink link = feed_link(run_index);

  // The generator opens with 16,384 adds (its default live-order target), so 40,000 events
  // leave room for executions and cancels.
  const std::vector<std::string> common = {"--group", link.group, "--port",        link.port,
                                           "--seed",  "7",        "--instruments", "16"};
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
  REQUIRE(reader.attach(bus, kStartTimeoutNs) == Status::Ok);

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

// SIGTERM ends the run as the end of stream does: exit 0, the summary printed, and the segment
// left with WriterState::Exited for its readers. Until then it runs: --max-ms 18446744073710,
// the smallest value whose nanoseconds pass 2^64, must not wrap to a 0.45 ms deadline.
TEST(sigterm_ends_the_run_cleanly) {
  const std::string bus = mdbus_test::make_test_bus_name("s");
  const std::string stdout_path = "/tmp/" + bus + ".out";
  std::vector<std::string> args = feed_handler_args(bus, feed_link(3));
  args.insert(args.end(), {"--max-ms", "18446744073710"});
  mdbus_test::ChildProcess feed(mdbus_test::spawn_program(args, stdout_path));
  REQUIRE(feed.pid > 0);
  BusReader<> reader;
  REQUIRE(reader.open(bus, kStartTimeoutNs) == Status::Ok);
  mdbus_test::sleep_ms(300);
  CHECK(load_writer_state(*reader.pointers().control) == WriterState::Running);

  REQUIRE(kill(feed.pid, SIGTERM) == 0);
  CHECK(feed.wait_for_exit_code() == kExitOk);
  CHECK(load_writer_state(*reader.pointers().control) == WriterState::Exited);
  CHECK(file_text(stdout_path).find("feed_handler summary") != std::string::npos);
  std::remove(stdout_path.c_str());
}

// --drop-count 2^64 - 1 must not wrap the drop window to nothing: of the 10 packets 100 events
// make, all from --drop-at 5 on are dropped (the simulator's summary counts them).
TEST(huge_drop_count_does_not_wrap) {
  const std::string stdout_path = "/tmp/tsim" + std::to_string(getpid()) + ".out";
  const FeedLink link = feed_link(5);
  mdbus_test::ChildProcess sim(mdbus_test::spawn_program(
      {MDBUS_EXCHANGE_SIM_PATH, "--group", link.group, "--port", link.port, "--events", "100",
       "--drop-at", "5", "--drop-count", "18446744073709551615"},
      stdout_path));
  CHECK(sim.wait_for_exit_code() == kExitOk);
  CHECK(file_text(stdout_path).find("packets=10 dropped=5 ") != std::string::npos);
  std::remove(stdout_path.c_str());
}

// A feed handler busy with packets that publish nothing still refreshes its heartbeat, so a quiet
// reader does not take it for Down. The backlog is queued while the handler is stopped; two
// adds that publish, 5000 packets apart, let this reader read the heartbeat twice inside it.
// Its summary counts what it dropped: each add goes out twice, and the copy (behind the next seq
// expected) is a duplicate, not a gap; the end of stream carries a cancel, never applied, so it
// is malformed and the book ends suspect.
TEST(busy_feed_handler_keeps_its_heartbeat) {
  constexpr std::uint32_t kPacketsBetweenMarks = 5000;  // a few 1 ms heartbeat periods of work
  // The idle path refreshes the heartbeat once the backlog is gone; these keep that well after
  // the second mark.
  constexpr std::uint32_t kPacketsAfterMarks = 1500;

  const std::string bus = mdbus_test::make_test_bus_name("h");
  const FeedLink link = feed_link(7);
  const std::string stdout_path = "/tmp/" + bus + ".out";
  std::vector<std::string> args = feed_handler_args(bus, link);
  args.insert(args.end(), {"--max-ms", "20000"});  // ends a run that misses its end of stream
  mdbus_test::ChildProcess feed(mdbus_test::spawn_program(args, stdout_path));
  REQUIRE(feed.pid > 0);
  BusReader<> reader;
  REQUIRE(reader.open(bus, kStartTimeoutNs) == Status::Ok);
  MulticastSender sender;
  REQUIRE(sender.open(link.address));

  // Stopped, so the whole backlog is queued before the handler reads any of it.
  REQUIRE(kill(feed.pid, SIGSTOP) == 0);
  int status = 0;
  REQUIRE(waitpid(feed.pid, &status, WUNTRACED) == feed.pid && WIFSTOPPED(status));

  // Instrument 0's reference price, from the default seed's list, as the feed handler has it.
  std::mt19937_64 random_engine(book::GeneratorConfig{}.seed);
  const std::int32_t reference_price =
      book::make_instrument_list(random_engine, 4)[0].reference_price;
  std::uint64_t seq = 1;
  // A mark: an add that becomes instrument 0's best bid, so the book publishes a delta.
  const auto send_mark = [&](std::int32_t ticks_below_reference) {
    book::OrderEvent add{};
    add.type = book::EventType::Add;
    add.order_id = seq;
    add.price = reference_price - ticks_below_reference;
    add.qty = 100;
    wire::PacketBuilder mark;
    mark.start(seq);
    mark.add(add);
    ++seq;
    return sender.send(mark.data(), mark.size()) && sender.send(mark.data(), mark.size());
  };
  // Packets of 10 cancels of orders the book does not have: work that publishes nothing.
  const auto send_cancels = [&](std::uint32_t packet_count) {
    for (std::uint32_t p = 0; p < packet_count; ++p) {
      wire::PacketBuilder cancels;
      cancels.start(seq);
      for (std::uint32_t e = 0; e < wire::kMaxMessagesPerPacket; ++e) {
        book::OrderEvent cancel{};
        cancel.type = book::EventType::Cancel;
        cancel.order_id = 1'000'000'000 + seq;
        cancels.add(cancel);
        ++seq;
      }
      if (!sender.send(cancels.data(), cancels.size())) return false;
    }
    return true;
  };
  REQUIRE(send_mark(10));
  REQUIRE(send_cancels(kPacketsBetweenMarks));
  REQUIRE(send_mark(9));
  REQUIRE(send_cancels(kPacketsAfterMarks));
  wire::PacketBuilder end;
  end.start_end_of_stream(seq);
  book::OrderEvent cancel{};
  cancel.type = book::EventType::Cancel;
  end.add(cancel);
  REQUIRE(sender.send(end.data(), end.size()));

  REQUIRE(kill(feed.pid, SIGCONT) == 0);
  const ControlBlock<StdAtomics>& control = *reader.pointers().control;
  const RingReader<StdAtomics, BusLayout<>> ring(reader.pointers().slots,
                                                 reader.pointers().control);
  REQUIRE(wait_until_published(ring, 0));
  const std::uint64_t heartbeat_at_first_mark =
      StdAtomics::load_acquire(control.liveness.heartbeat_ns);
  REQUIRE(wait_until_published(ring, 1));
  const std::uint64_t heartbeat_at_second_mark =
      StdAtomics::load_acquire(control.liveness.heartbeat_ns);
  std::cerr << "  heartbeat moved "
            << static_cast<double>(heartbeat_at_second_mark - heartbeat_at_first_mark) / 1e6
            << " ms between the marks\n";
  CHECK(heartbeat_at_second_mark > heartbeat_at_first_mark);
  CHECK(feed.wait_for_exit_code() == kExitOk);  // its end of stream
  const std::string summary = file_text(stdout_path);
  CHECK(summary.find("malformed=1 ") != std::string::npos);
  CHECK(summary.find("gaps=0  missing=0  duplicates=2  book_suspect=yes") != std::string::npos);
  std::remove(stdout_path.c_str());
}

// mdbus_watch --destroy refuses while a writer holds the bus: the segment stays and a second
// writer is still refused. Once the writer has closed, --destroy removes the bus. A bus name that
// breaks the rules is a command-line error.
TEST(watch_destroy_refuses_while_a_writer_holds_the_bus) {
  REQUIRE(access(MDBUS_WATCH_PATH, X_OK) == 0);
  const std::string bus = mdbus_test::make_test_bus_name("w");
  BusWriter<> writer;
  REQUIRE(writer.open(bus, 4) == Status::Ok);

  CHECK(run_watch_destroy(bus) == kExitSetupFailed);
  BusReader<> reader;
  CHECK(reader.open(bus) == Status::Ok);
  BusWriter<> second_writer;
  CHECK(second_writer.open(bus, 4) == Status::AnotherWriterRunning);

  writer.close();
  CHECK(run_watch_destroy(bus) == kExitOk);
  BusReader<> after_destroy;
  CHECK(after_destroy.open(bus) == Status::NoSuchBus);
  CHECK(run_watch_destroy("my.bus") == kExitBadCommandLine);
}
