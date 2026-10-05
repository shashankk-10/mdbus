// The dispatch decision in isolation: the cost of getting one payload into the reader's work.
// - static: Schema::dispatch into a template handler, what Consumer<Derived> compiles to.
// - virtual: the same handler behind an interface pointer, as for a handler registered at run
//   time.
// - In-process over the encoded payload pool, in timed batches: no bus, no readers, no spinning.
// - Exit codes: 0 ok, 2 bad option or the row could not be written, 4 smoke run measured
//   nothing.
//   mdbus_bench_dispatch --dispatch static|virtual --duration-ms T --out DIR

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench_args.hpp"
#include "measurement.hpp"
#include "result_row.hpp"
#include "workload.hpp"

using namespace mdbus;
using namespace mdbus::bench;

namespace {

using PayloadPool = EncodedPayloadPool<BusLayout<>>;

constexpr std::uint64_t kMessagesPerBatch = 16384;  // enough to hide the two counter reads
constexpr std::size_t kMaxBatches = 65536;  // the run ends early once they are all filled

constexpr double kDefaultDurationMs = 2000;  // --duration-ms

constexpr int kExitBadUsage = 2;      // bad option, or the row could not be written
constexpr int kExitSmokeFailed = 4;   // --smoke and the run measured nothing

// SimulatedReaderWork behind an interface, as for a handler registered at run time.
struct DispatchHandler {
  SimulatedReaderWork work;

  virtual ~DispatchHandler() = default;

  virtual void on(const BookDelta& delta, std::uint16_t instrument_id) {
    work.on(delta, instrument_id);
  }

  virtual void on(const Trade& trade, std::uint16_t instrument_id) {
    work.on(trade, instrument_id);
  }

  virtual void on(const InstrumentStatus& status, std::uint16_t instrument_id) {
    work.on(status, instrument_id);
  }
};

// Dispatches message_count pool payloads through the virtual interface.
// - Each measured loop is its own noinline function, so its code is not specialised for the
//   call site.
[[gnu::noinline]] void run_virtual(DispatchHandler* handler, const PayloadPool& pool,
                                   std::uint64_t message_count) {
  // The compiler cannot see what a volatile holds, so it cannot work out the handler's real
  // type and turn the virtual calls into direct ones.
  DispatchHandler* volatile opaque_handler = handler;
  DispatchHandler* const indirect_handler = opaque_handler;
  for (std::uint64_t k = 0; k < message_count; ++k) {
    Payload<kDefaultPayloadWords> payload;
    std::memcpy(&payload, pool[k % kEventPoolSize].data(), sizeof payload);
    DefaultSchema::dispatch(payload.header.type_id, payload.body, [&](const auto& message) {
      indirect_handler->on(message, payload.header.instrument_id);
    });
  }
}

// Dispatches message_count pool payloads statically, as Consumer<Derived> does.
[[gnu::noinline]] void run_static(SimulatedReaderWork& work, const PayloadPool& pool,
                                  std::uint64_t message_count) {
  for (std::uint64_t k = 0; k < message_count; ++k) {
    dispatch_to_work(work, pool[k % kEventPoolSize]);
  }
}

// The row just written, in two lines.
void print_dispatch_summary(const ResultRow& row) {
  std::printf("%s dispatch (%s): %.2f ns, %.0f instructions per message\n",
              row.text("variant").c_str(), row.text("dispatch").c_str(), row.number("dispatch_ns"),
              row.number("dispatch_instr"));
  print_conditions_line(row);
}

}  // namespace

int main(int argc, char** argv) {
  const BenchArgs args{argc, argv};
  const std::string out = args.text("--out", ".");
  const std::string mode = args.text("--dispatch", "static");
  const auto duration_ms =
      static_cast<std::uint64_t>(args.number("--duration-ms", kDefaultDurationMs));
  const auto seed = static_cast<std::uint64_t>(args.number("--seed", 1));

  if (mode != "static" && mode != "virtual") {
    std::fprintf(stderr, "--dispatch static|virtual\n");
    return kExitBadUsage;
  }
  const bool virtual_dispatch = mode == "virtual";

  static PayloadPool pool;  // one in ten a trade, as in the bus benchmark
  encode_event_pool<BusLayout<>>(make_event_pool(seed), pool);
  static DispatchHandler handler;
  prefer_p_cores();
  prefault_stack();
  TimedBatches batches(kMaxBatches);

  // 1. Time batches until the duration ends or every batch slot is filled.
  const ProcessCounters start = ProcessCounters::read();
  const std::uint64_t end_tick = read_ticks() + ms_to_ticks(duration_ms);
  while (read_ticks() < end_tick && !batches.full()) {
    batches.time_batch(kMessagesPerBatch, [&] {
      if (virtual_dispatch)
        run_virtual(&handler, pool, kMessagesPerBatch);
      else
        run_static(handler.work, pool, kMessagesPerBatch);
    });
  }
  const ProcessCounters counters = ProcessCounters::read() - start;

  // 2. Gates.
  std::string failed_gates;
  add_failed_gate(failed_gates, is_release_build(), "env:debug_build");
  add_cpu_gates(failed_gates, counters);

  // 3. The row. pshare_writer and ghz_writer are named "writer" so report.py reads them like the
  //    bus rows; work goes in the row so the compiler cannot drop the handler's sums.
  ResultRow row;
  row.set_text("variant", "Dispatch");
  row.set_text("scenario", "dispatch");
  row.set_text("dispatch", mode);
  row.set_integer("seed", seed);
  row.set_number("dispatch_ns", batches.median_ns());
  row.set_number("dispatch_instr", batches.median_instructions());
  row.set_number("dispatch_cycles", batches.median_cycles());
  row.set_number("pshare_writer", counters.p_core_share());
  row.set_number("ghz_writer", counters.effective_ghz());
  row.set_integer("work", handler.work.traded);

  const bool wrote = finish_row(row, failed_gates, out);
  print_dispatch_summary(row);
  if (args.number("--smoke", 0) != 0 && row.number("dispatch_ns") <= 0) return kExitSmokeFailed;
  if (!wrote) return kExitBadUsage;
  return 0;
}
