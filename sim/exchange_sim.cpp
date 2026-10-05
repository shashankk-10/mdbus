// mdbus_exchange_sim: the fake exchange, where the feed starts.
// - Takes order-by-order events from OrderEventGenerator, packs up to 10 per packet in the
//   ITCH-shaped format of wire_format.hpp, and multicasts them on 127.0.0.1 only.
// - Sends at a fixed packet rate (--rate). Between packets it sleeps until the next send time
//   instead of spinning, so it does not take a core away from the feed handler.
// - --drop-at I --drop-count N fakes packet loss: packets I to I+N-1 (counted from 0) are built
//   and numbered but not sent. Example: end_to_end_test passes --drop-at 300 --drop-count 2, so
//   with 10 events per packet seqs 3001 to 3020 never arrive and the feed handler sees one gap
//   of 20 messages.
// - Exit codes (src/command_line.hpp): kExitOk, kExitSendFailed, kExitBadCommandLine,
//   kExitSetupFailed.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "mdbus/clock.hpp"
#include "mdbus/feed/multicast_socket.hpp"
#include "mdbus/feed/wire_format.hpp"
#include "sim/order_event_generator.hpp"
#include "src/command_line.hpp"

using namespace mdbus;

namespace {

using SteadyClock = std::chrono::steady_clock;

// 3.2 ms at 20,000 packets/s. Past that (after a stall, say the process was stopped), the
// schedule restarts from now instead of sending the missed packets back to back.
constexpr std::int64_t kMaxPacketsBehindSchedule = 64;

// The end-of-stream packet is sent this many times, this far apart, so that losing one copy
// does not leave the feed handler waiting forever.
constexpr int kEndOfStreamCopies = 3;
constexpr std::chrono::milliseconds kEndOfStreamResendGap{1};

// The simulator's settings, all from the command line.
struct ExchangeConfig {
  sockaddr_in group_address{};                // --group ADDR --port P, both required
  std::uint64_t event_count = 10'000;         // --events: how many events to send in all
  std::uint64_t packets_per_second = 20'000;  // --rate
  std::uint64_t first_dropped_packet = 0;     // --drop-at: the first packet not sent, from 0
  std::uint64_t dropped_packet_count = 0;     // --drop-count: how many in a row are not sent
  book::GeneratorConfig generator_config;     // --seed, --instruments: same as the feed handler
};

// Fills config from the command line. False on an unknown option, a bad value, or a missing
// --group or --port.
bool parse_command_line(int argc, char** argv, ExchangeConfig& config) {
  return parse_feed_command_line(
      argc, argv, config.group_address, [&](const std::string& key, const std::string& value) {
        if (key == "--events") return parse_unsigned(value, config.event_count);
        if (key == "--rate") {
          return parse_unsigned(value, config.packets_per_second) && config.packets_per_second > 0;
        }
        if (key == "--drop-at") return parse_unsigned(value, config.first_dropped_packet);
        if (key == "--drop-count") return parse_unsigned(value, config.dropped_packet_count);
        std::uint64_t number = 0;  // --seed or --instruments
        return parse_unsigned(value, number) && config.generator_config.set_option(key, number);
      });
}

// Printed when the command line is wrong.
void print_help() {
  const ExchangeConfig defaults;
  const book::GeneratorConfig& generator = defaults.generator_config;
  std::cerr
      << "Sends simulated exchange order events by UDP multicast, on this machine only.\n"
      << "\n"
      << "usage: mdbus_exchange_sim --group ADDR --port PORT [options]\n"
      << "\n"
      << "required:\n"
      << "  --group ADDR       multicast address to send to, 224.0.0.0 to 239.255.255.255\n"
      << "  --port PORT        UDP port, 1 to 65535\n"
      << "\n"
      << "options:\n"
      << "  --events N         how many events to send in all (default " << defaults.event_count
      << ")\n"
      << "  --rate N           packets per second, up to " << wire::kMaxMessagesPerPacket
      << " events each (default " << defaults.packets_per_second << ")\n"
      << "  --seed N           random seed for the events (default " << generator.seed << ")\n"
      << "  --instruments N    how many instruments, 1 to 65534 (default "
      << generator.instrument_count << ")\n"
      << "                     The feed handler must be given the same --seed and --instruments.\n"
      << "\n"
      << "to test packet loss:\n"
      << "  --drop-at I        the first packet not to send, counting from 0\n"
      << "  --drop-count N     how many packets in a row not to send (default 0: none)\n"
      << "\n"
      << "example:\n"
      << "  mdbus_exchange_sim --group 239.255.0.1 --port 30001 --events 40000\n";
}

// The exchange's outgoing feed: numbers the generator's events from 1, packs them into packets,
// sends each packet at its time, and leaves out the packets in the drop window.
class ExchangeFeed {
 private:
  const ExchangeConfig config;
  MulticastSender sender;
  book::OrderEventGenerator generator;
  wire::PacketBuilder packet_builder;
  const std::chrono::nanoseconds send_interval;
  SteadyClock::time_point next_send_time;
  std::uint64_t next_seq = 1;
  std::uint64_t packets_built = 0;
  std::uint64_t packets_dropped = 0;
  std::uint64_t send_errors = 0;

 public:
  explicit ExchangeFeed(const ExchangeConfig& settings)
      : config(settings),
        generator(settings.generator_config),
        send_interval(static_cast<std::int64_t>(kNsPerSecond / settings.packets_per_second)) {}

  bool open() {
    return sender.open(config.group_address);
  }

  // Sends every event, then the end of stream. False if any send failed.
  bool run() {
    next_send_time = SteadyClock::now();
    while (next_seq <= config.event_count) {
      build_next_packet();
      wait_for_send_time();
      send_or_drop();
    }
    send_end_of_stream();
    return send_errors == 0;
  }

  void print_summary() const {
    std::cout << "exchange_sim packets=" << packets_built << " dropped=" << packets_dropped
              << " events=" << config.event_count << " send_errors=" << send_errors << std::endl;
  }

 private:
  // Fills packet_builder with the next events, up to kMaxMessagesPerPacket of them. Seqs count
  // events from 1, so the header's first_seq is the seq of the packet's first event.
  void build_next_packet() {
    packet_builder.start(next_seq);
    while (packet_builder.message_count() < wire::kMaxMessagesPerPacket &&
           next_seq <= config.event_count) {
      packet_builder.add(generator.next_event());
      ++next_seq;
    }
  }

  // Sleeps until this packet's slot in the schedule; if the sender has fallen more than
  // kMaxPacketsBehindSchedule packets behind, the schedule restarts from now.
  void wait_for_send_time() {
    std::this_thread::sleep_until(next_send_time);
    next_send_time += send_interval;
    const SteadyClock::time_point now = SteadyClock::now();
    if (now > next_send_time + kMaxPacketsBehindSchedule * send_interval) next_send_time = now;
  }

  // Sends the built packet, unless its number is in the drop window. A dropped packet still uses
  // up its seqs, so the receiver sees a gap.
  // - The window test subtracts instead of adding, so a --drop-count up to 2^64 - 1 cannot wrap.
  void send_or_drop() {
    const bool drop = packets_built >= config.first_dropped_packet &&
                      packets_built - config.first_dropped_packet < config.dropped_packet_count;
    if (drop) {
      ++packets_dropped;
    } else if (!sender.send(packet_builder.data(), packet_builder.size())) {
      ++send_errors;
    }
    ++packets_built;
  }

  // Tells the receivers the run is over. Never dropped, whatever the drop window says.
  void send_end_of_stream() {
    wire::PacketBuilder end_of_stream_packet;
    end_of_stream_packet.start_end_of_stream(next_seq);
    for (int copy = 0; copy < kEndOfStreamCopies; ++copy) {
      if (copy != 0) std::this_thread::sleep_for(kEndOfStreamResendGap);
      if (!sender.send(end_of_stream_packet.data(), end_of_stream_packet.size())) ++send_errors;
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  ExchangeConfig config;
  if (!parse_command_line(argc, argv, config)) {
    print_help();
    return kExitBadCommandLine;
  }

  ExchangeFeed feed(config);
  if (!feed.open()) return kExitSetupFailed;
  const bool all_sent = feed.run();
  feed.print_summary();
  if (!all_sent) return kExitSendFailed;
  return kExitOk;
}
