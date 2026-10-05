// The feed ladder: ns and instructions per event of FeedBook<BookConfig>, replaying the
// generator's L3 (order-by-order) stream in-process.
// - One book configuration per binary (F0..F4, from baseline/book_ladder.hpp; CMake sets
//   MDBUS_FEED_VARIANT).
// - The stream is generated up front and copied in 64 KB chunks into a small arena, one timed
//   batch per chunk, so streaming the input never evicts the book from cache.
// - A fresh book reaches steady state through an untimed warm-up. Timed batches go through
//   apply_batch, so the prefetch step runs as it does in the feed handler.
// - Gates: F0 and F1 allocate per event, so they are exempt from the page-fault gate. A run whose
//   book refused any add is invalid: the refusal path would be timed instead of the book.
// - Exit codes: 0 ok, 2 the row could not be written, 4 smoke run measured nothing.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "baseline/book_ladder.hpp"
#include "bench_args.hpp"
#include "measurement.hpp"
#include "result_row.hpp"
#include "sim/order_event_generator.hpp"

using namespace mdbus;
using namespace mdbus::bench;
using BookConfig = baseline::MDBUS_FEED_VARIANT;
using BenchBook = book::FeedBook<BookConfig>;

namespace {

constexpr int kExitRowNotWritten = 2;
constexpr int kExitSmokeFailed = 4;  // --smoke and the run measured nothing

// Half the P-core's 128 KB L1D: the chunk and the book's hot lines both fit.
constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr std::size_t kEventsPerChunk = kChunkBytes / sizeof(book::OrderEvent);

// Untimed warm-up events per live order: twice the live target fills the book and walks every
// structure before the first timed batch.
constexpr std::size_t kWarmupEventsPerLiveOrder = 2;

constexpr double kDefaultLiveOrders = 16384;  // --live: resting orders the generator aims for
constexpr double kDefaultEventCount = 1e6;    // --events: timed events per run

// The row just written, in two lines.
void print_feed_summary(const ResultRow& row) {
  std::printf("%s feed: %.1f ns/event, %.0f instructions/event\n", row.text("variant").c_str(),
              row.number("ns_per_event"), row.number("instr_per_event"));
  print_conditions_line(row);
}

}  // namespace

int main(int argc, char** argv) {
  const BenchArgs args{argc, argv};
  const std::string out = args.text("--out", ".");
  book::GeneratorConfig config;
  config.seed = static_cast<std::uint64_t>(args.number("--seed", 1));
  config.target_live_orders = static_cast<std::uint32_t>(args.number("--live", kDefaultLiveOrders));
  const auto events = static_cast<std::size_t>(args.number("--events", kDefaultEventCount));

  // 1. Generate the warm-up and timed events up front.
  const std::size_t warmup_events =
      kWarmupEventsPerLiveOrder * std::size_t{config.target_live_orders};

  // Open addressing at load 0.5 or less.
  const unsigned order_table_log2 = book::order_table_size_log2_for(config.target_live_orders);

  book::OrderEventGenerator generator(config);
  std::vector<book::OrderEvent> stream(warmup_events + events);
  generator.fill_events(stream.data(), stream.size());

  const auto feed_book = std::make_unique<BenchBook>(generator.instrument_list(), order_table_log2);
  // 2. Warm the book up, untimed.
  for (std::size_t i = 0; i < warmup_events; ++i) feed_book->apply(stream[i]);

  // 3. Set up the process and the arena so the timed part cannot fault.
  prefer_p_cores();
  static book::OrderEvent arena[kEventsPerChunk];
  prefault_and_lock(arena, sizeof arena);
  TimedBatches batches(events / kEventsPerChunk + 1);
  std::uint64_t deltas = 0;
  prefault_stack();

  // 4. Replay: copy one chunk into the arena (untimed), then time the book over it. The deltas
  //    count keeps the book's output alive.
  const ProcessCounters start = ProcessCounters::read();
  for (std::size_t i = warmup_events; i < stream.size(); i += kEventsPerChunk) {
    const std::size_t chunk_events = std::min(kEventsPerChunk, stream.size() - i);
    std::memcpy(arena, &stream[i], chunk_events * sizeof(book::OrderEvent));
    auto count = [&](std::size_t, const book::BookOutput& output) {
      deltas += output.delta.entry_count;
    };
    batches.time_batch(chunk_events, [&] { feed_book->apply_batch(arena, chunk_events, count); });
  }
  const ProcessCounters counters = ProcessCounters::read() - start;

  // 5. Gates, then the row.
  std::string failed_gates;
  add_failed_gate(failed_gates, is_release_build(), "env:debug_build");
  add_cpu_gates(failed_gates, counters);
  add_failed_gate(failed_gates,
                  counters.minor_page_faults == 0 || baseline::kAllocatesPerEvent<BookConfig>,
                  "minflt");
  add_failed_gate(failed_gates, feed_book->counters().refused_adds == 0, "rejects");

  ResultRow row;
  row.set_text("variant", BookConfig::kName);
  row.set_text("scenario", "feed");
  row.set_integer("events", events);
  row.set_integer("live", config.target_live_orders);
  row.set_integer("seed", config.seed);
  row.set_integer("deltas", deltas);
  row.set_number("ns_per_event", batches.median_ns());
  row.set_number("instr_per_event", batches.median_instructions());
  row.set_number("cycles_per_event", batches.median_cycles());
  // Named "writer" like the bus rows, so report.py reads the CPU gates' inputs the same way.
  row.set_number("pshare_writer", counters.p_core_share());
  row.set_number("ghz_writer", counters.effective_ghz());
  row.set_integer("minflt_max", counters.minor_page_faults);

  const bool wrote = finish_row(row, failed_gates, out);
  print_feed_summary(row);
  if (args.number("--smoke", 0) != 0 && row.number("ns_per_event") <= 0) return kExitSmokeFailed;
  if (!wrote) return kExitRowNotWritten;
  return 0;
}
