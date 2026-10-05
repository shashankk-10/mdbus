// The bus benchmark: one binary per variant (baseline/bus_variants.hpp). It writes one row.csv
// per run.
// - Run with no --role, it is the launcher (launch() below). It spawns the roles below from this
//   same executable, each in its own process, and talks to them through a BenchControl segment.
// - Two scenarios. Paced (--rate > 0): open-loop, message i is due at a fixed tick whether or
//   not the writer kept up. W-sat (--rate 0): unpaced, the writer publishes flat out.
// - hop = read tick - publish_ticks: the ring hop, since the fast reader polls RingReader
//   directly and none of Consumer's per-message work is in it. e2e = read tick -
//   latency_start_ticks (the due tick, so it also counts a writer running late).
// - Exit codes: bench_args.hpp's, and src/command_line.hpp's kExitSetupFailed for a role that
//   could not open its bus or histograms.
//
// The roles:
//   writer  P-core. Publishes paced, or in W-sat in timed batches of 16384 publishes.
//   fast    P-core. Records hop and e2e. In W-sat it only copies and validates. --fast 0
//           leaves it out: the writer's cost with no reader at all.
//   slow    E-core, --slow read: 500 ns of work per message, so at 1 M msg/s it is about half
//           busy, keeps up and laps only now and then (laps_slow). Between messages it spins
//           on the slot the writer writes next, as every reader here does: a caught-up,
//           spin-waiting E-core reader, not the SleepWait slow reader of DESIGN.md §4.9. Reads
//           one snapshot every 64 messages (snap_mean_ns).

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "baseline/bus_variants.hpp"
#include "bench_args.hpp"
#include "bus_bench_control.hpp"
#include "child_processes.hpp"
#include "measurement.hpp"
#include "result_row.hpp"
#include "workload.hpp"

using namespace mdbus;
using namespace mdbus::baseline;
using namespace mdbus::bench;

using Variant = variants::MDBUS_VARIANT;
using Bus = VariantBus<Variant>;
using PayloadWords = Bus::Layout::PayloadWords;

namespace {

constexpr std::uint32_t kInstrumentCount = kDefaultInstrumentCount;

// Workload.
constexpr std::uint64_t kPublishesPerBatch = 16384;      // W-sat publishes per batch
constexpr std::uint64_t kMessagesPerSnapshotWrite = 16;  // paced: one snapshot per 16 messages
constexpr std::uint64_t kMessagesPerSnapshotRead = 64;   // the slow reader reads one per 64
constexpr std::uint64_t kSlowReaderWorkNs = 500;         // the slow reader's work per message
constexpr std::uint64_t kMessagesPerWindowCheck = 1024;  // W-sat readers read the clock this often
constexpr std::uint64_t kIdlePollsPerStopCheck = 256;    // idle polls between stop and drain checks

// Polling sleeps, outside the window, so their size only sets how fast a role reacts.
constexpr unsigned kGoPollMicroseconds = 100;      // a ready role checks for go this often
constexpr unsigned kDrainSleepMicroseconds = 500;  // a finished writer heartbeats this often

// Launcher timeouts. A role that misses one aborts the run.
constexpr std::uint64_t kWriterReadyTimeoutMs = 10'000;   // writer's bus created and mapped
constexpr std::uint64_t kReadersReadyTimeoutMs = 15'000;  // readers opened and prefaulted
constexpr std::uint64_t kRolesDoneTimeoutMs = 10'000;     // after the window: readers drained

constexpr const char* kBenchBusPrefix = "bk";  // + the launcher's pid: "bk12345"
// A publish that starts more than one tick after its due tick counts as late.
constexpr std::uint64_t kLateSlackTicks = 1;
constexpr std::size_t kMaxTimedBatches = 262144;  // W-sat alone fills about 25,000 a second

// The command line (bench_args.hpp). --role and --name are the launcher's, for its roles.
constexpr const char* kUsage =
    "[--rate MSG_PER_S, 0 for W-sat] [--duration-ms MS] [--warmup-ms MS] [--seed N] [--fast 0|1] "
    "[--slow off|read] [--smoke 0|1] [--out DIR]";
constexpr OptionRule kOptionRules[] = {
    {.key = "--rate"},
    {.key = "--duration-ms", .min_value = 1, .max_value = kMaxPhaseMs},
    {.key = "--warmup-ms", .max_value = kMaxPhaseMs},
    {.key = "--seed"},
    {.key = "--fast", .max_value = 1},
    {.key = "--smoke", .max_value = 1},
    {.key = "--slow", .choices = "off|read"},
};

// Mapped by every process: the launcher creates it, each role maps it in main.
BenchControl* bench_control = nullptr;
// The progress of the bus the readers read (set in main).
// - A global, not a parameter of read_until_done: as a parameter it costs the fast reader's
//   per-message path an instruction (checked in the disassembly).
const BusProgress* read_progress = nullptr;

// The command line. Roles get the launcher's flags plus --role and --name (child_processes.hpp).
struct Options {
  std::string role;  // empty in the launcher
  std::string name;  // the hot bus name; the launcher picks it
  std::string out_dir;
  std::string slow_mode;  // off or read
  double rate;            // messages per second; 0 means W-sat
  std::uint64_t duration_ms;  // the window
  std::uint64_t warmup_ms;    // before it
  std::uint64_t seed;
  bool with_fast_reader;
  bool is_smoke_run;

  // Every value was checked against kOptionRules (BenchArgs::text), so each cast is exact.
  explicit Options(const BenchArgs& args)
      : role(args.text("--role", "")),
        name(args.text("--name", "")),
        out_dir(args.text("--out", ".")),
        slow_mode(args.text("--slow", "off")),
        rate(args.number("--rate", 1e6)),
        duration_ms(static_cast<std::uint64_t>(args.number("--duration-ms", 2000))),
        warmup_ms(static_cast<std::uint64_t>(args.number("--warmup-ms", 300))),
        seed(static_cast<std::uint64_t>(args.number("--seed", 1))),
        with_fast_reader(args.number("--fast", 1) != 0),
        is_smoke_run(args.number("--smoke", 0) != 0) {}
};

// A role's sleep while it polls the control segment: before go, and the writer's while readers
// drain. A role whose launcher died (it was adopted by launchd, pid 1) exits here instead of
// waiting forever.
// - Out of line, and shaped like usleep: main inlines the roles, and an orphan check inlined in
//   their wait loops changes the code around their measured loops.
[[gnu::noinline]] void poll_sleep(useconds_t microseconds) {
  usleep(microseconds);
  if (getppid() == 1) std::exit(kExitRunFailed);
}

// Reports ready, then sleeps until go. False if the launcher stopped the run first.
bool report_ready_and_wait_for_go(RoleResults& result) {
  result.state.store(kReady, std::memory_order_release);
  while (bench_control->command.go.load(std::memory_order_acquire) == 0) {
    if (bench_control->command.stop.load(std::memory_order_acquire) != 0) return false;
    poll_sleep(kGoPollMicroseconds);
  }
  return true;
}

void mark_done(RoleResults& result) {
  result.state.store(kDone, std::memory_order_release);
}

// Relaxed: only polled, and nothing read after it depends on the launcher's earlier writes.
bool stop_requested() {
  return bench_control->command.stop.load(std::memory_order_relaxed) != 0;
}

// The writer's measurement window: process counters, the next seq when it opened, and the
// writer's first and last ticks in it.
struct WriterSpan {
  CounterWindow counter_window;
  std::uint64_t seq_at_start = 0;
  std::uint64_t first_tick = 0;
  std::uint64_t last_tick = 0;
};

// Paced writer loop: publishes message i at its due tick on the open-loop schedule (due_tick).
// - Each message's latency start is its due tick, and a late publish counts in late_publishes.
// - Every kMessagesPerSnapshotWrite messages it also writes one snapshot, for the slow reader.
WriterSpan publish_paced(Bus::Writer& writer, const std::vector<SyntheticDelta>& events,
                         double rate, RoleResults& result) {
  const std::uint64_t window_start_tick = bench_control->command.window_start_tick;
  const std::uint64_t window_end_tick = bench_control->command.window_end_tick;
  const double period_ticks = static_cast<double>(kTicksPerSecond) / rate;
  WriterSpan span;

  for (std::uint64_t i = 0;; ++i) {
    const std::uint64_t due = due_tick(bench_control->command.go_tick, period_ticks, i);
    if (due >= window_end_tick) break;
    if (!span.counter_window.is_open && due >= window_start_tick) {
      span.counter_window.start();
      span.seq_at_start = writer.next_seq();
      span.first_tick = due;
    }

    const SyntheticDelta& event = events[i % kEventPoolSize];
    std::uint64_t now = read_ticks();
    while (now < due) now = read_ticks();
    if (span.counter_window.is_open && now > due + kLateSlackTicks) ++result.late_publishes;
    writer.publish(event.instrument_id, event.delta, due);

    // Snapshots go in the slack after the release, for the slow reader's snapshot reads.
    if ((i + 1) % kMessagesPerSnapshotWrite == 0) {
      InstrumentSnapshot snapshot{};
      snapshot.last_included_seq = writer.next_seq() - 1;
      snapshot.exchange_time_ns = due;
      snapshot.instrument_id = event.instrument_id;
      snapshot.bid_count = 1;
      snapshot.bids[0] = event.delta.entries[0];
      writer.write_snapshot(event.instrument_id, snapshot);
    }
  }

  // A writer behind schedule published past window_end_tick.
  span.last_tick = std::max(window_end_tick, read_ticks());
  return span;
}

// W-sat writer loop: publishes flat out, in batches of kPublishesPerBatch from the pre-encoded
// pool.
// - Batch timing: the clock and the counters are read once per batch, not per publish, so the
//   per-publish cost is a batch's time divided by its size. TimedBatches keeps only batches that
//   lie wholly inside the window and reports their medians.
// - Encoding happened before go, so a batch times the publish and nothing else.
WriterSpan publish_unpaced(Bus::Writer& writer, const EncodedPayloadPool<Bus::Layout>& pool,
                           TimedBatches& batches) {
  const std::uint64_t window_start_tick = bench_control->command.window_start_tick;
  const std::uint64_t window_end_tick = bench_control->command.window_end_tick;
  WriterSpan span;
  std::uint64_t batch_end_tick = 0;

  while (batch_end_tick < window_end_tick) {
    if (!span.counter_window.is_open && read_ticks() >= window_start_tick) {
      span.counter_window.start();
      span.seq_at_start = writer.next_seq();
      span.first_tick = read_ticks();
    }
    const auto publish_batch = [&] {
      for (std::uint64_t k = 0; k < kPublishesPerBatch; ++k)
        writer.publish_encoded_words(pool[k % kEventPoolSize]);
    };
    batch_end_tick =
        batches.time_batch(kPublishesPerBatch, publish_batch, window_start_tick, window_end_tick);
    span.last_tick = batch_end_tick;
  }
  return span;
}

// The writer role: opens the hot bus, publishes until the window ends, reports what it did.
int run_writer(const Options& options, RoleResults& result) {
  // 1. Set up: P-core, the bus, the workload, the prefaulted stack.
  prefer_p_cores();
  // The role's long-lived objects are static: at a fixed address and off the prefaulted stack.
  static Bus::Writer writer;
  if (!writer.open(options.name, kInstrumentCount)) return kExitSetupFailed;

  // The workload is generated and encoded before the window: no generation sits inside a
  // W-sat batch.
  const std::vector<SyntheticDelta> events = make_event_pool(options.seed);
  static EncodedPayloadPool<Bus::Layout> pool;  // kEventPoolSize payloads
  encode_event_pool<Bus::Layout>(events, pool);
  TimedBatches batches(kMaxTimedBatches);
  prefault_stack();

  // 2. Tell readers where to start, report ready, wait for go.
  bench_control->hot_bus_progress.first_seq.store(writer.next_seq(), std::memory_order_relaxed);
  if (!report_ready_and_wait_for_go(result)) return 0;

  // 3. Publish until window_end_tick.
  WriterSpan span;
  if (options.rate > 0) {
    span = publish_paced(writer, events, options.rate, result);
  } else {
    span = publish_unpaced(writer, pool, batches);
  }

  // 4. Publish final_seq, then writer_done with release, so a reader that sees writer_done
  //    also sees final_seq. Then the results.
  result.process_counters = span.counter_window.end();
  bench_control->hot_bus_progress.final_seq.store(writer.next_seq(), std::memory_order_relaxed);
  bench_control->hot_bus_progress.writer_done.store(1, std::memory_order_release);
  result.messages_published = writer.next_seq() - span.seq_at_start;
  if (span.last_tick > span.first_tick) {
    const double window_ticks = static_cast<double>(span.last_tick - span.first_tick);
    const double published = static_cast<double>(result.messages_published);
    result.achieved_rate = published * static_cast<double>(kTicksPerSecond) / window_ticks;
  }
  result.wsat_ns_per_publish = batches.median_ns();
  result.wsat_instructions_per_publish = batches.median_instructions();
  result.wsat_cycles_per_publish = batches.median_cycles();
  mark_done(result);

  // 5. Stay alive, heartbeating, until the launcher says stop (poll_sleep exits once it has
  //    died), so the bus keeps a live writer while readers drain. No reader here checks the
  //    writer's liveness, but a Consumer would see an exited writer as Down.
  while (!stop_requested()) {
    writer.heartbeat_if_due();
    poll_sleep(kDrainSleepMicroseconds);
  }
  return 0;
}

// The read loop every reader role shares. Returns at stop, or once the writer is done and
// every message up to final_seq has been read or lost.
// - Ok: on_message(payload), then the next seq.
// - Lapped: count the lap and the lost messages, resume at the writer's head hint (at least
//   one past the lost seq, so a stale hint cannot loop).
// - NotWrittenYet: reader.idle(), a spin (or the next locked poll, for Mutex). Every
//   kIdlePollsPerStopCheck idle polls it also opens the counter window if due, and checks stop
//   and drain. Busy reading, it checks neither, which keeps the clock read off the per-message
//   path.
template <class MessageHandler>
void read_until_done(Bus::Reader& reader, RoleResults& result, CounterWindow& counter_window,
                     MessageHandler&& on_message) {
  PayloadWords payload{};
  std::uint64_t cursor = read_progress->first_seq.load(std::memory_order_relaxed);
  std::uint64_t idle_polls = 0;

  while (true) {
    const TryPollResult poll_result = reader.poll(cursor, payload);
    if (poll_result == TryPollResult::Ok) {
      idle_polls = 0;
      ++cursor;
      ++result.messages_read;
      on_message(payload);
    } else if (poll_result == TryPollResult::Lapped) {
      const std::uint64_t resume_at = std::max(reader.resume_seq(), cursor + 1);
      ++result.times_lapped;
      result.messages_lost += resume_at - cursor;
      cursor = resume_at;
    } else {
      reader.idle();
      ++idle_polls;
      if (idle_polls < kIdlePollsPerStopCheck) continue;
      idle_polls = 0;
      if (read_ticks() >= bench_control->command.window_start_tick) counter_window.start();
      if (stop_requested()) return;
      const bool writer_done = read_progress->writer_done.load(std::memory_order_acquire) != 0;
      const std::uint64_t final_seq = read_progress->final_seq.load(std::memory_order_relaxed);
      if (writer_done && cursor >= final_seq) return;
    }
  }
}

// The fast reader role: a P-core reader of the hot bus that records hop and e2e (paced only).
// - Only messages whose latency start lies inside the window are sampled.
// - A sample with read < publish or publish < latency start counts as a causality violation
//   and is not recorded; the causality gate fails the run if there is any.
int run_fast(const Options& options, RoleResults& result) {
  prefer_p_cores();
  // static, as in run_writer. As stack locals, the histograms and the work change the compiled
  // per-message loop (checked in the disassembly), and every recorded run used this one.
  static Bus::Reader reader;
  static LatencyHistogram hop;
  static LatencyHistogram e2e;
  static SimulatedReaderWork work;
  if (!reader.open(options.name)) return kExitSetupFailed;
  if (!hop.init() || !e2e.init()) return kExitSetupFailed;
  prefault_stack();
  if (!report_ready_and_wait_for_go(result)) return 0;

  const std::uint64_t window_start_tick = bench_control->command.window_start_tick;
  const std::uint64_t window_end_tick = bench_control->command.window_end_tick;
  CounterWindow counter_window;
  read_until_done(reader, result, counter_window, [&](const PayloadWords& payload) {
    if (options.rate <= 0) {  // W-sat: copy and validate only
      if (result.messages_read % kMessagesPerWindowCheck == 0 &&
          read_ticks() >= window_start_tick) {
        counter_window.start();
      }
      return;
    }

    const std::uint64_t read_tick = read_ticks();  // right after validation, before dispatch
    dispatch_to_work(work, payload);
    const std::uint64_t latency_start_ticks = payload[kLatencyStartTicksWord];
    const std::uint64_t publish_ticks = payload[kPublishTicksWord];
    if (publish_ticks == 0 || latency_start_ticks < window_start_tick ||
        latency_start_ticks >= window_end_tick) {
      return;
    }
    counter_window.start();
    ++result.latency_samples;
    if (read_tick < publish_ticks || publish_ticks < latency_start_ticks) {
      ++result.causality_violations;
      return;
    }
    hop.record(read_tick - publish_ticks);
    e2e.record(read_tick - latency_start_ticks);
  });

  result.process_counters = counter_window.end();
  result.hop = hop.summary();
  result.e2e = e2e.summary();
  mark_done(result);
  return 0;
}

// The slow reader role, on an E-core: 500 ns of work per message, a caught-up spin-waiting
// reader (see the file header). It times one snapshot read every kMessagesPerSnapshotRead
// messages.
int run_slow(const Options& options, RoleResults& result) {
  const std::uint64_t work_ticks = ns_to_ticks(kSlowReaderWorkNs);
  static Bus::Reader reader;  // static, as in run_fast
  static LatencyHistogram snapshot_read_latency;
  if (!reader.open(options.name)) return kExitSetupFailed;
  if (!snapshot_read_latency.init()) return kExitSetupFailed;
  prefault_stack();
  if (!report_ready_and_wait_for_go(result)) return 0;

  const std::uint64_t window_start_tick = bench_control->command.window_start_tick;
  const std::uint64_t window_end_tick = bench_control->command.window_end_tick;
  InstrumentSnapshot snapshot{};
  CounterWindow counter_window;
  read_until_done(reader, result, counter_window, [&](const PayloadWords&) {
    const std::uint64_t before_read = read_ticks();
    if (before_read >= window_start_tick) counter_window.start();
    if (result.messages_read % kMessagesPerSnapshotRead == 0 && reader.has_snapshots()) {
      const auto instrument_id = static_cast<std::uint16_t>(
          result.messages_read / kMessagesPerSnapshotRead % kInstrumentCount);
      const SnapshotReadResult read_result = reader.read_snapshot(instrument_id, snapshot);
      const std::uint64_t after_read = read_ticks();
      if (before_read >= window_start_tick && after_read < window_end_tick) {
        snapshot_read_latency.record(after_read - before_read);
        ++result.snapshot_reads;
        result.snapshot_read_retries += read_result.retries;
        if (read_result.status == SnapshotReadStatus::GaveUp) ++result.snapshot_reads_gave_up;
      }
    }
    spin_until_tick(read_ticks() + work_ticks);
  });

  result.process_counters = counter_window.end();
  result.snapshot_read_latency = snapshot_read_latency.summary();
  mark_done(result);
  return 0;
}

// Starts the roles in order: the writer first, since readers start at the seq it publishes
// once its bus is open. False if a role did not start or did not report ready.
bool start_roles(const Options& options, const BenchArgs& args, const std::string& name,
                 std::vector<RoleProcess>& role_processes, std::string& aborted) {
  auto start = [&](Role role, bool e_core) {
    RoleProcess process;
    process.role = role;
    process.pid = spawn(args, role, name, e_core);
    role_processes.push_back(process);
    return process.pid > 0;
  };

  if (!start(kWriter, false)) return false;
  if (!wait_until_all(*bench_control, role_processes, kReady, kWriterReadyTimeoutMs, aborted))
    return false;
  if (options.with_fast_reader && !start(kFastReader, false)) return false;
  if (options.slow_mode != "off" && !start(kSlowReader, true)) return false;
  return wait_until_all(*bench_control, role_processes, kReady, kReadersReadyTimeoutMs, aborted);
}

// The eight columns of one latency histogram, named prefix_n, prefix_mean_ns and so on.
void set_latency_columns(ResultRow& row, const std::string& prefix,
                         const LatencySummary& summary) {
  row.set_integer(prefix + "_n", summary.sample_count);
  row.set_number(prefix + "_mean_ns", summary.mean * kNsPerTick);
  row.set_number(prefix + "_mean_body_ns", summary.body_mean * kNsPerTick);
  row.set_number(prefix + "_trim_frac", summary.share_above_body_cutoff);
  row.set_integer(prefix + "_p50", summary.p50);
  row.set_integer(prefix + "_p99", summary.p99);
  row.set_integer(prefix + "_p999", summary.p999);
  row.set_integer(prefix + "_max", summary.max);
}

// Copies the roles' results into the row's columns.
void write_result_columns(const Options& options, const std::vector<RoleProcess>& role_processes,
                          ResultRow& row) {
  const RoleResults& writer = bench_control->role_results[kWriter];
  const RoleResults& fast = bench_control->role_results[kFastReader];
  const RoleResults& slow = bench_control->role_results[kSlowReader];

  row.set_integer("published", writer.messages_published);
  row.set_number("rate_achieved", writer.achieved_rate);
  row.set_integer("writer_late", writer.late_publishes);
  if (options.rate <= 0) {
    row.set_number("wsat_ns", writer.wsat_ns_per_publish);
    row.set_number("wsat_instr", writer.wsat_instructions_per_publish);
    row.set_number("wsat_cycles", writer.wsat_cycles_per_publish);
  }

  row.set_integer("msgs_fast", fast.messages_read);
  row.set_integer("sampled", fast.latency_samples);
  row.set_integer("laps_fast", fast.times_lapped);
  row.set_integer("lost_fast", fast.messages_lost);
  row.set_integer("causality", fast.causality_violations);
  set_latency_columns(row, "hop", fast.hop);
  set_latency_columns(row, "e2e", fast.e2e);

  row.set_integer("msgs_slow", slow.messages_read);
  row.set_integer("laps_slow", slow.times_lapped);
  row.set_integer("snap_reads", slow.snapshot_reads);
  row.set_integer("snap_retries", slow.snapshot_read_retries);
  row.set_integer("snap_busy", slow.snapshot_reads_gave_up);
  row.set_number("snap_mean_ns", slow.snapshot_read_latency.mean * kNsPerTick);
  row.set_integer("snap_p50", slow.snapshot_read_latency.p50);
  row.set_integer("snap_p99", slow.snapshot_read_latency.p99);

  std::uint64_t minflt_max = 0;
  for (const RoleProcess& process : role_processes) {
    const ProcessCounters& counters = bench_control->role_results[process.role].process_counters;
    const std::string role = kRoleNames[process.role];
    row.set_number("ghz_" + role, counters.effective_ghz());
    row.set_number("pshare_" + role, counters.p_core_share());
    minflt_max = std::max(minflt_max, counters.minor_page_faults);
  }
  row.set_integer("minflt_max", minflt_max);
}

// The gates on the roles' results: each one that fails adds its name to failed_gates, and
// finish_row then writes the row with valid = 0.
// - minflt_<role>: no minor page faults inside the window.
// - pshare_<role>, ghz_<role>: the P-core roles really ran on a P-core at full clock.
// - causality, fast_laps: below.
void check_gates(const Options& options, const std::vector<RoleProcess>& role_processes,
                 std::string& failed_gates) {
  const RoleResults& fast = bench_control->role_results[kFastReader];

  for (const RoleProcess& process : role_processes) {
    const ProcessCounters& counters = bench_control->role_results[process.role].process_counters;
    const std::string role = kRoleNames[process.role];
    add_failed_gate(failed_gates, counters.read_ok && counters.minor_page_faults == 0,
                    "minflt_" + role);
    if (process.role != kSlowReader) add_cpu_gates(failed_gates, counters, ("_" + role).c_str());
  }

  add_failed_gate(failed_gates, fast.causality_violations == 0, "causality");
  // Paced runs must not lap. An unpaced writer outruns a mutex reader by design, so W-sat
  // reports laps and lost messages instead of gating on them.
  add_failed_gate(failed_gates, options.rate <= 0 || fast.times_lapped == 0, "fast_laps");
}

// --smoke: whether the run measured something, whatever the gates said.
// - W-sat: the writer timed some batches (wsat_ns).
// - Paced: the fast reader took latency samples (hop_n), if there was a fast reader.
bool measured_something(const Options& options, const ResultRow& row) {
  if (options.rate <= 0) return row.number("wsat_ns") > 0;
  return !options.with_fast_reader || row.number("hop_n") > 0;
}

// Prints the row just written, in a few lines, for a person at the terminal.
void print_bus_summary(const ResultRow& row) {
  const std::string scenario = row.text("scenario");
  std::printf("%s %s", row.text("variant").c_str(), scenario.c_str());
  if (scenario == "wsat") {
    std::printf(": unpaced, %.0f msg/s", row.number("rate_achieved"));
  } else {
    std::printf(": %.0f msg/s offered, %.0f achieved", row.number("rate_offered"),
                row.number("rate_achieved"));
  }

  const auto on_off = [&row](const char* column) { return row.text(column) == "1" ? "on" : "off"; };
  std::printf("; fast reader %s, slow reader %s\n", on_off("fast"), row.text("slow").c_str());

  if (scenario == "wsat") {
    std::printf("  writer: %.2f ns, %.0f instructions per publish\n", row.number("wsat_ns"),
                row.number("wsat_instr"));
  }

  if (row.number("hop_n") > 0) {
    const double body_cutoff_ns = static_cast<double>(kBodyMeanCutoffTicks) * kNsPerTick;
    std::printf(
        "  fast reader hop: body mean %.1f ns (all messages %.1f ns; %.2f%% were over "
        "%.0f ns)\n",
        row.number("hop_mean_body_ns"), row.number("hop_mean_ns"),
        100 * row.number("hop_trim_frac"), body_cutoff_ns);
    std::printf(
        "  p50 %s; p99 %s; p99.9 %s; e2e p99 %s\n", format_ticks(row.number("hop_p50")).c_str(),
        format_ticks(row.number("hop_p99")).c_str(), format_ticks(row.number("hop_p999")).c_str(),
        format_ticks(row.number("e2e_p99")).c_str());
  }
  print_conditions_line(row);
}

// Removes the names of everything a run creates: the bus (segment and lock file) and the
// control segment. Removing a name that does not exist does nothing.
void remove_names(const std::string& name, const std::string& control_name) {
  destroy_bus(name);
  shm_unlink(control_name.c_str());
}

// The launcher: runs the roles once and writes row.csv. Returns one of the exit codes in the
// file header.
int launch(const Options& options, const BenchArgs& args) {
  // 1. The row's identity columns, and the two gates on the machine itself.
  ResultRow row;
  row.set_text("variant", MDBUS_VARIANT_NAME);
  row.set_text("scenario", options.rate > 0 ? "paced" : "wsat");
  row.set_number("rate_offered", options.rate);
  row.set_integer("fast", options.with_fast_reader ? 1 : 0);
  row.set_text("slow", options.slow_mode);
  row.set_integer("duration_ms", options.duration_ms);
  row.set_integer("seed", options.seed);

  std::string failed_gates;  // any gate failed: the row is written with valid = 0
  std::string aborted;       // why the run did not complete
  add_failed_gate(failed_gates, is_release_build(), "env:debug_build");
  add_failed_gate(failed_gates, verify_counter_is_24mhz(), "clock");

  // 2. Create the control segment, named after the hot bus (which is named after our pid).
  const std::string name = kBenchBusPrefix + std::to_string(getpid());
  const std::string control_name = kShmNamePrefix + name + kBenchControlSuffix;
  shm_unlink(control_name.c_str());
  SharedMemoryMapping control_segment;
  const std::size_t control_bytes = round_up_to_multiple(sizeof(BenchControl), kPageSize);
  if (control_segment.create(control_name, control_bytes) != Status::Ok) {
    aborted = "control_segment";
  } else {
    // The launcher constructs the control block in the new segment; roles map it and cast.
    bench_control = new (control_segment.address()) BenchControl();

    // 3. Start the roles. Once all are ready, every role has mapped what it needs, so the names
    //    go (a mapping outlives its name, and a launcher that dies from here on leaks nothing).
    //    Then set the window and go, and sleep through it.
    std::vector<RoleProcess> role_processes;
    const bool started = start_roles(options, args, name, role_processes, aborted);
    if (started) {
      remove_names(name, control_name);
      LauncherCommand& command = bench_control->command;
      command.go_tick = read_ticks();
      command.window_start_tick = command.go_tick + ms_to_ticks(options.warmup_ms);
      command.window_end_tick = command.window_start_tick + ms_to_ticks(options.duration_ms);
      command.go.store(1, std::memory_order_release);
      usleep(static_cast<useconds_t>((options.warmup_ms + options.duration_ms) *
                                     kMicrosecondsPerMillisecond));
      // The writer stops at window_end_tick; readers drain.
      wait_until_all(*bench_control, role_processes, kDone, kRolesDoneTimeoutMs, aborted);
    } else if (aborted.empty()) {
      aborted = "spawn";
    }

    // 4. Stop and reap every role. Unless the run already aborted, the first one that exited
    //    non-zero or never reported done aborts it.
    bench_control->command.stop.store(1, std::memory_order_release);
    for (RoleProcess& process : role_processes) {
      const int exit_code = reap(process.pid);
      const RoleResults& results = bench_control->role_results[process.role];
      const bool done = results.state.load(std::memory_order_acquire) == kDone;
      if (started && aborted.empty() && (exit_code != 0 || !done)) {
        aborted = std::string("role_failed:") + kRoleNames[process.role] + ":";
        aborted += std::to_string(exit_code);
      }
    }

    // 5. Results and gates.
    write_result_columns(options, role_processes, row);
    check_gates(options, role_processes, failed_gates);
  }

  // 6. Remove the names (a run that failed to start still has them), then write and print the
  //    row.
  remove_names(name, control_name);
  const bool wrote = finish_row(row, failed_gates, options.out_dir, aborted);
  print_bus_summary(row);

  if (options.is_smoke_run && !measured_something(options, row)) return kExitMeasuredNothing;
  if (!wrote || !aborted.empty()) return kExitRunFailed;
  return 0;
}

}  // namespace

// No --role: run as the launcher. With --role: map the launcher's control segment and run
// that role.
int main(int argc, char** argv) {
  const BenchArgs args{argc, argv};
  const Options options(args);
  if (options.role.empty()) return launch(options, args);

  SharedMemoryMapping control_segment;
  const std::string control_name = kShmNamePrefix + options.name + kBenchControlSuffix;
  const Status opened = control_segment.open(control_name, Access::ReadWrite, sizeof(BenchControl));
  if (opened != Status::Ok) return kExitRunFailed;
  bench_control = static_cast<BenchControl*>(control_segment.address());
  read_progress = &bench_control->hot_bus_progress;

  if (options.role == "writer") return run_writer(options, bench_control->role_results[kWriter]);
  if (options.role == "fast") return run_fast(options, bench_control->role_results[kFastReader]);
  if (options.role == "slow") return run_slow(options, bench_control->role_results[kSlowReader]);
  return kExitRunFailed;
}

const CommandLineRules mdbus::bench::kCommandLineRules{kUsage, kOptionRules};
