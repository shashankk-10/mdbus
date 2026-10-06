// mdbus_watch: the live viewer of a named bus, and the smallest complete Consumer.
// - Once a second it prints that second's messages, book deltas and trades, the running totals
//   of status messages and laps, the writer's health and one instrument's best bid and ask,
//   read from the snapshot table.
// - If the feed handler restarts, it makes a fresh segment; once the old writer reads Down, the
//   viewer attaches to the new one by name. Ctrl-C ends the process; --destroy removes a bus
//   instead of watching it.

#include <iomanip>
#include <iostream>
#include <string>

#include "mdbus/bus_paths.hpp"
#include "mdbus/consumer.hpp"
#include "mdbus/writer_liveness.hpp"
#include "src/command_line.hpp"

using namespace mdbus;

namespace {

constexpr std::uint64_t kAttachTimeoutNs = 10'000'000'000;

// The viewer's settings, all from the command line. --destroy takes no value; every other option
// is a "--name value" pair.
struct WatchConfig {
  std::string bus_name;             // --bus NAME, required
  std::uint64_t instrument_id = 0;  // --inst: whose top of book to show
  std::uint64_t seconds = 0;        // --seconds: 0 runs until Ctrl-C
  bool destroy_requested = false;   // --destroy: remove the bus instead of watching it
};

// False on an unknown option, a bad or missing value, or a missing --bus.
bool parse_command_line(int argc, char** argv, WatchConfig& config) {
  bool command_line_ok = true;
  for (int i = 1; command_line_ok && i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--destroy") {
      config.destroy_requested = true;
      continue;
    }
    if (i + 1 == argc) return false;  // the option's value is missing
    ++i;
    const std::string value = argv[i];
    if (key == "--bus") {
      config.bus_name = value;
    } else if (key == "--inst") {
      command_line_ok = parse_unsigned(value, config.instrument_id);
    } else if (key == "--seconds") {
      command_line_ok = parse_unsigned(value, config.seconds);
    } else {
      command_line_ok = false;
    }
  }
  return command_line_ok && !config.bus_name.empty();
}

// The help text, printed when the command line is wrong.
void print_help() {
  std::cerr
      << "Watches a named bus: once a second, what arrived and one instrument's top of book.\n"
      << "\n"
      << "usage: mdbus_watch --bus NAME [options]\n"
      << "\n"
      << "options:\n"
      << "  --inst N           the instrument whose best bid and ask to show (default 0)\n"
      << "  --seconds N        stop after N seconds (default 0: until Ctrl-C)\n"
      << "  --destroy          remove the bus (its segment and lock file) and exit;\n"
      << "                     refused while a writer holds the bus\n"
      << "\n"
      << "example:\n"
      << "  mdbus_watch --bus demo --inst 3\n";
}

// Counts each message type; the counts feed the report line. Why SleepWait, not SpinWait:
// - a viewer must not take a core from the feed handler or the fast readers;
// - a millisecond of backlog is far below the ring's 16,384 slots, so sleeping never laps it.
struct CountingConsumer : Consumer<CountingConsumer, SleepWait> {
  std::uint64_t deltas = 0;
  std::uint64_t trades = 0;
  std::uint64_t statuses = 0;

  // One on() per schema type, as Consumer requires; the viewer only counts them. The book
  // itself is not rebuilt here: the top of book comes from the snapshot table instead.
  void on(const BookDelta&, const MessageInfo&) {
    ++deltas;
  }

  void on(const Trade&, const MessageInfo&) {
    ++trades;
  }

  void on(const InstrumentStatus&, const MessageInfo&) {
    ++statuses;
  }
};

std::string best_level_text(const Level& level, unsigned levels_on_side) {
  if (levels_on_side == 0) return "-";
  return std::to_string(level.qty) + " @ " + std::to_string(level.price);
}

// The best bid and ask from the instrument's snapshot.
std::string top_of_book_text(const CountingConsumer& consumer, std::uint64_t instrument_id) {
  // Checked again on every line: a restarted feed handler may publish fewer instruments.
  if (instrument_id >= consumer.instrument_count()) return "not on this bus";
  InstrumentSnapshot snapshot;
  const SnapshotReadStatus read_status =
      consumer.read_snapshot(static_cast<std::uint16_t>(instrument_id), snapshot).status;
  if (read_status != SnapshotReadStatus::Ok) return "no snapshot";

  std::string text = "bid " + best_level_text(snapshot.bids[0], snapshot.bid_count);
  text += " | ask " + best_level_text(snapshot.asks[0], snapshot.ask_count);
  if ((snapshot.instrument_flags & kInstrumentBad) != 0) text += "  BAD";
  if ((snapshot.instrument_flags & kInstrumentSuspect) != 0) text += "  SUSPECT";
  return text;
}

// The whole viewer: attach, then poll and print one line a second.
class BusWatcher {
 private:
  const WatchConfig config;
  CountingConsumer consumer;
  // Counts at the last report line, so each line shows one second's worth.
  std::uint64_t messages_read = 0;
  std::uint64_t reported_messages = 0;
  std::uint64_t reported_deltas = 0;
  std::uint64_t reported_trades = 0;

 public:
  explicit BusWatcher(const WatchConfig& settings) : config(settings) {}

  // Attaches, waiting up to 10 s for the bus to appear. False if it did not.
  bool open() {
    const Status status = consumer.attach(config.bus_name, kAttachTimeoutNs);
    if (status != Status::Ok) {
      std::cerr << "mdbus_watch: bus " << config.bus_name << ": status " << status_name(status)
                << std::endl;
      return false;
    }
    return true;
  }

  // After open(): false if --inst is not on this bus.
  bool instrument_is_on_bus() const {
    const std::uint32_t instrument_count = consumer.instrument_count();
    if (config.instrument_id >= instrument_count) {
      std::cerr << "mdbus_watch: --inst must be below " << instrument_count << std::endl;
      return false;
    }
    return true;
  }

  // For --seconds, or until Ctrl-C ends the process.
  void run() {
    std::cout << "watching bus " << config.bus_name << ": " << consumer.instrument_count()
              << " instruments, top of book for instrument " << config.instrument_id << std::endl;
    std::uint64_t next_report_ns = steady_clock_ns() + kNsPerSecond;
    std::uint64_t second = 1;
    while (config.seconds == 0 || second <= config.seconds) {
      if (consumer.poll_once() == PollOnceResult::GotMessage) ++messages_read;
      if (steady_clock_ns() < next_report_ns) continue;
      follow_restarted_writer();
      print_report_line(second);
      next_report_ns += kNsPerSecond;
      ++second;
    }
  }

 private:
  // While the writer reads Down, looks for a live one under the bus name: a restarted feed
  // handler's fresh segment. The dead writer's segment stays linked until then, and its old
  // heartbeat keeps it from being attached again.
  void follow_restarted_writer() {
    if (consumer.writer_health() != WriterHealth::Down) return;
    BusReader<> candidate;
    if (candidate.open(config.bus_name) != Status::Ok) return;
    const std::uint64_t now_ns = steady_clock_ns();
    if (judge_writer(*candidate.pointers().control, now_ns, UINT64_MAX, kHeartbeatTimeoutNs) !=
        WriterHealth::Alive)
      return;
    if (consumer.attach(config.bus_name) == Status::Ok)
      std::cout << "attached to a new writer's segment" << std::endl;
  }

  void print_report_line(std::uint64_t second) {
    std::cout << std::setw(3) << second << " s " << std::setw(8)
              << messages_read - reported_messages << " msg/s " << std::setw(7)
              << consumer.deltas - reported_deltas << " deltas/s " << std::setw(6)
              << consumer.trades - reported_trades << " trades/s  (total " << consumer.statuses
              << " status msgs, " << consumer.stats().times_lapped << " laps)  writer "
              << health_name(consumer.writer_health()) << "  inst_id " << config.instrument_id
              << ": " << top_of_book_text(consumer, config.instrument_id) << std::endl;
    reported_messages = messages_read;
    reported_deltas = consumer.deltas;
    reported_trades = consumer.trades;
  }
};

// --destroy: removes the bus's segment and lock file while holding the writer lock, so a writer
// that holds the bus, running or stopped, makes it refuse. A bus that does not exist is fine.
int destroy_bus_without_writer(const std::string& bus_name) {
  BusPaths paths;
  WriterLock writer_lock;
  Status status = Status::BadName;
  if (make_bus_paths(bus_name, paths)) {
    status = writer_lock.acquire(paths);
  }
  if (status != Status::Ok) {
    std::cerr << "mdbus_watch: bus " << bus_name << ": status " << status_name(status)
              << ", not removed" << std::endl;
    return status == Status::BadName ? kExitBadCommandLine : kExitSetupFailed;
  }
  destroy_bus(bus_name);
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  WatchConfig config;
  if (!parse_command_line(argc, argv, config)) {
    print_help();
    return kExitBadCommandLine;
  }
  if (config.destroy_requested) return destroy_bus_without_writer(config.bus_name);

  BusWatcher watcher(config);
  if (!watcher.open()) return kExitSetupFailed;
  if (!watcher.instrument_is_on_bus()) return kExitBadCommandLine;
  watcher.run();
  return kExitOk;
}
