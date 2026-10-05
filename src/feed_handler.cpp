// mdbus_feed_handler: the one writer of a named bus, the middle of
// exchange (mdbus_exchange_sim) -> feed handler -> FeedBook -> Publisher -> ring -> Consumer.
// - Per packet: decode (wire_format.hpp; a malformed packet is dropped whole, a message of
//   unknown type is skipped), sequence check, then one batch through FeedBook::apply_batch and
//   publish_book_output.
// - Gap policy: nothing is retransmitted, so the first gap marks every instrument
//   kInstrumentSuspect for the rest of the run.
// - Takes the simulator's --seed and --instruments, because both derive the instrument list
//   from them.
// - The end-of-stream packet or --max-ms ends the run, then a summary of every stage is
//   printed. Exit codes: kExitOk, kExitBadCommandLine, kExitSetupFailed (command_line.hpp).

#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "mdbus/book/feed_book.hpp"
#include "mdbus/book/publish_book_output.hpp"
#include "mdbus/bus_writer.hpp"
#include "mdbus/feed/multicast_socket.hpp"
#include "mdbus/feed/wire_format.hpp"
#include "command_line.hpp"

using namespace mdbus;

namespace {

// One spare byte: an oversized datagram fills the buffer and fails to decode instead of being cut
// to a valid-looking size.
constexpr std::size_t kDatagramBufferBytes = wire::kMaxPacketBytes + 1;

// The feed handler's settings, all from the command line. Every option is a "--name value" pair.
struct FeedHandlerConfig {
  std::string bus_name;          // --bus NAME, required
  sockaddr_in group_address{};   // --group ADDR --port P, both required
  std::uint64_t max_run_ms = 0;  // --max-ms: 0 runs until the end of stream
  // --seed, --instruments: the same as the simulator's
  book::GeneratorConfig generator_config;
};

// Fills config from argv. False on an unknown option, a bad value, or a missing --bus, --group
// or --port.
bool parse_command_line(int argc, char** argv, FeedHandlerConfig& config) {
  std::string group_text;
  std::uint16_t port = 0;
  bool command_line_ok = argc % 2 == 1;  // every option takes a value
  for (int i = 1; command_line_ok && i < argc; i += 2) {
    const std::string key = argv[i];
    const std::string value = argv[i + 1];
    if (key == "--bus") {
      config.bus_name = value;
    } else if (key == "--group") {
      group_text = value;
    } else if (key == "--port") {
      command_line_ok = parse_port(value, port);
    } else if (key == "--max-ms") {
      command_line_ok = parse_unsigned(value, config.max_run_ms);
    } else {
      command_line_ok = parse_seed_or_instruments(key, value, config.generator_config);
    }
  }
  return command_line_ok && !config.bus_name.empty() && port != 0 &&
         make_multicast_address(group_text, port, config.group_address);
}

void print_help() {
  const book::GeneratorConfig generator;
  std::cerr
      << "Receives the simulated exchange feed by UDP multicast, keeps the order book, and\n"
      << "publishes it on a named bus.\n"
      << "\n"
      << "usage: mdbus_feed_handler --bus NAME --group ADDR --port PORT [options]\n"
      << "\n"
      << "required:\n"
      << "  --bus NAME         the bus to create (replaces one left by an earlier run)\n"
      << "  --group ADDR       multicast address to listen on, 224.0.0.0 to 239.255.255.255\n"
      << "  --port PORT        UDP port, 1 to 65535\n"
      << "\n"
      << "options:\n"
      << "  --max-ms T         stop after T ms even without an end of stream (default 0: never)\n"
      << "  --seed N           the simulator's --seed (default " << generator.seed << ")\n"
      << "  --instruments N    the simulator's --instruments (default "
      << generator.instrument_count << ")\n"
      << "\n"
      << "example:\n"
      << "  mdbus_feed_handler --bus demo --group 239.255.0.1 --port 30001\n"
      << "  prints \"ready bus=demo\" once it listens, and a summary per stage at the end.\n";
}

// The simulator's instrument list, derived from the same seed (make_instrument_list).
std::vector<book::InstrumentInfo> instrument_list_for(const book::GeneratorConfig& config) {
  std::mt19937_64 random_engine(config.seed);
  return book::make_instrument_list(random_engine, config.instrument_count);
}

// The book and the publisher it feeds, plus the feed-level flags the book does not own.
struct BookPublisher {
  book::FeedBook<> feed_book;
  Publisher<>& publisher;
  std::uint32_t feed_flags = 0;  // ORed into every snapshot from now on
  std::uint64_t slots_published = 0;

  BookPublisher(const std::vector<book::InstrumentInfo>& instrument_list, unsigned order_log2,
                Publisher<>& bus_publisher)
      : feed_book(instrument_list, order_log2), publisher(bus_publisher) {}

  // Applies one packet's events and publishes each output. receive_ticks goes on the first slot
  // the batch publishes, so the latency measured is receive to first publish.
  void apply_and_publish(const book::OrderEvent* events, std::size_t count,
                         std::uint64_t receive_ticks) {
    std::uint64_t receive_ticks_left = receive_ticks;
    feed_book.apply_batch(events, count, [&](std::size_t, const book::BookOutput& output) {
      if (!output.has_anything_to_publish()) return;
      slots_published +=
          book::publish_book_output(publisher, feed_book, output, feed_flags, receive_ticks_left);
      receive_ticks_left = 0;
    });
  }

  // Marks every instrument kInstrumentSuspect, each announced with an InstrumentStatus and its
  // flagged snapshot.
  // - Runs once, on the first gap; later gaps return at once.
  void mark_all_instruments_suspect() {
    if (feed_flags != 0) return;
    feed_flags = kInstrumentSuspect;

    for (std::uint32_t i = 0; i < feed_book.instrument_count(); ++i) {
      const auto instrument_id = static_cast<std::uint16_t>(i);
      InstrumentSnapshot snapshot;
      feed_book.fill_snapshot(instrument_id, snapshot);
      snapshot.instrument_flags |= feed_flags;
      book::publish_instrument_status(publisher, instrument_id, snapshot, book::RejectReason::None);
      ++slots_published;
    }
  }
};

// What arrived from the exchange, before the sequence check.
struct ReceiveCounters {
  std::uint64_t packets_received = 0;
  std::uint64_t malformed_packets = 0;         // dropped whole: they failed to decode
  std::uint64_t skipped_unknown_messages = 0;  // a type the decoder does not know
};

// The whole program: the socket, the bus and the book, and the receive loop.
class FeedHandler {
 private:
  const FeedHandlerConfig config;
  MulticastReceiver receiver;
  BusWriter<> writer;
  BookPublisher book_publisher;
  wire::SequenceGapTracker tracker;
  wire::DecodedPacket packet;
  ReceiveCounters received;
  std::uint8_t datagram[kDatagramBufferBytes];

 public:
  explicit FeedHandler(const FeedHandlerConfig& settings)
      : config(settings),
        book_publisher(
            instrument_list_for(settings.generator_config),
            book::order_table_size_log2_for(settings.generator_config.target_live_orders),
            writer.publisher()) {}

  // The socket opens before the bus, so a reader that has attached knows the feed is listening.
  // False if either fails.
  bool open() {
    if (!receiver.open(config.group_address)) return false;
    const Status status = writer.open(config.bus_name, config.generator_config.instrument_count);
    if (status != Status::Ok) {
      std::cerr << "mdbus_feed_handler: bus " << config.bus_name << ": status "
                << status_name(status) << std::endl;
      return false;
    }
    std::cout << "ready bus=" << config.bus_name << std::endl;  // flushed: scripts wait for it
    return true;
  }

  // Receives and applies packets until the end-of-stream packet, or --max-ms.
  void run() {
    const std::uint64_t start_ns = steady_clock_ns();
    while (true) {
      const std::size_t bytes = receiver.try_receive(datagram, sizeof datagram);
      if (bytes == 0) {
        // Nothing queued. A quiet writer must still look alive to its readers.
        writer.publisher().heartbeat_if_due();
        const std::uint64_t run_ns = steady_clock_ns() - start_ns;
        if (config.max_run_ms != 0 && run_ns >= config.max_run_ms * kNsPerMillisecond) return;
        continue;
      }

      const std::uint64_t receive_ticks = read_ticks();  // the receive time, read in user space
      ++received.packets_received;
      if (!wire::decode_packet(datagram, bytes, packet)) {
        ++received.malformed_packets;
        continue;
      }

      const wire::SequenceGapTracker::PacketOrder order =
          tracker.check_packet(packet.first_seq, packet.message_count);
      if (order == wire::SequenceGapTracker::kGap) book_publisher.mark_all_instruments_suspect();
      // Tested before the duplicate check, so an end packet is never skipped as a duplicate:
      // whichever of the simulator's 3 copies arrives first ends the run.
      if (packet.end_of_stream) return;
      if (order == wire::SequenceGapTracker::kDuplicate) continue;
      received.skipped_unknown_messages += packet.skipped_unknown_messages;
      book_publisher.apply_and_publish(packet.events, packet.event_count, receive_ticks);
    }
  }

  // The run's summary, one line per stage, in the order a packet passes through them.
  void print_summary() const {
    const book::BookCounters& book = book_publisher.feed_book.counters();
    std::cout << "feed_handler summary\n"
              << "  received   packets=" << received.packets_received
              << "  malformed=" << received.malformed_packets
              << "  unknown_types=" << received.skipped_unknown_messages << "\n"
              << "  sequence   gaps=" << tracker.gap_count()
              << "  missing=" << tracker.missing_message_count()
              << "  book_suspect=" << (tracker.has_seen_gap() ? "yes" : "no") << "\n"
              << "  book       events=" << book.events_applied
              << "  top_changes=" << book.top_level_changes << "  rejects=" << book.refused_adds
              << "  unknown_instruments=" << book.unknown_instruments
              << "  unknown_orders=" << book.unknown_orders << "\n"
              << "  published  slots=" << book_publisher.slots_published << std::endl;
  }
};

}  // namespace

int main(int argc, char** argv) {
  FeedHandlerConfig config;
  if (!parse_command_line(argc, argv, config)) {
    print_help();
    return kExitBadCommandLine;
  }

  FeedHandler feed_handler(config);
  if (!feed_handler.open()) return kExitSetupFailed;
  feed_handler.run();
  feed_handler.print_summary();
  return kExitOk;  // the segments stay for readers; the next writer replaces them
}
