#pragma once

// What a run measures besides its schedule.
// - LatencyHistogram: hop and e2e latency in ticks, summarised into LatencySummary rows.
// - TimedBatches: per-operation ns, instructions and cycles of fixed-size batches.
// - ProcessCounters: the per-process counters the validity gates read (P-core share, clock,
//   page faults); CounterWindow: their change over a measurement window.
// - Process setup: QoS, prefaulting and wiring memory, so the page-fault gate passes.

#include <hdr/hdr_histogram.h>
#include <libproc.h>
#include <pthread/qos.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "timing.hpp"

namespace mdbus::bench {

// Process setup.

// Above 16 ticks (667 ns) a hop is an OS interruption, not the protocol: such samples count in
// the tail percentiles but not in the body mean, so one stall cannot swamp a comparison.
constexpr std::uint64_t kBodyMeanCutoffTicks = ns_to_ticks(667);

// Asks for a P-core by raising this thread to the highest QoS class. macOS has no core pinning,
// so the P-core share gate checks afterwards whether it worked.
inline void prefer_p_cores() {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

// Writes and wires byte_count bytes, so touching them later cannot fault.
// - The page-fault gate (minflt == 0 over the window) passes by construction: memory a window
//   uses is written and wired before the window opens, and so is the stack it may grow into.
inline void prefault_and_lock(void* bytes, std::size_t byte_count) {
  std::memset(bytes, 0, byte_count);
  mlock(bytes, byte_count);  // failure only means an idle page may be compressed; the gate shows it
}

constexpr std::size_t kStackBytesToPrefault = 256 * 1024;  // more stack than any role uses
// Smallest page on any Apple machine, so touching every 4 KB touches every page.
constexpr std::size_t kSmallestPageSize = 4096;

// Touches kStackBytesToPrefault of stack below the caller, so the role's later frames are mapped.
inline void prefault_stack() {
  volatile unsigned char stack_bytes[kStackBytesToPrefault];
  for (std::size_t i = 0; i < sizeof stack_bytes; i += kSmallestPageSize) stack_bytes[i] = 0;
}

// Latency histogram.

// Latency summary in ticks, handed between processes through shared memory.
struct LatencySummary {
  std::uint64_t sample_count = 0;
  std::uint64_t p50 = 0;
  std::uint64_t p99 = 0;
  std::uint64_t p999 = 0;
  std::uint64_t max = 0;
  double mean = 0;
  double body_mean = 0;                // the mean of the samples at or below kBodyMeanCutoffTicks
  double share_above_body_cutoff = 0;  // the share of samples above kBodyMeanCutoffTicks
};

constexpr std::int64_t kHistogramMinTicks = 1;
constexpr std::int64_t kHistogramMaxTicks = static_cast<std::int64_t>(kTicksPerSecond) * 3600;
constexpr int kHistogramSignificantDigits = 3;

// Latency samples in ticks, with percentiles from HdrHistogram and exact means kept beside it.
// - Range [1 tick, 1 hour] at 3 significant digits: exact 1-tick bins up to 2,047 ticks, and
//   no sample is ever out of range.
// - Samples above kBodyMeanCutoffTicks count in the percentiles and the mean, not body_mean.
class LatencyHistogram {
 private:
  // Owns the C library's histogram: the unique_ptr calls hdr_close on it when it goes.
  std::unique_ptr<hdr_histogram, void (*)(hdr_histogram*)> histogram{nullptr, hdr_close};
  std::uint64_t sample_count = 0;
  std::uint64_t sum = 0;
  std::uint64_t body_count = 0;
  std::uint64_t body_sum = 0;

 public:
  // Allocates the histogram and prefaults its counts, so record() never faults. False if
  // HdrHistogram could not allocate.
  bool init() {
    hdr_histogram* created = nullptr;
    if (hdr_init(kHistogramMinTicks, kHistogramMaxTicks, kHistogramSignificantDigits,
                 &created) != 0)
      return false;
    histogram.reset(created);
    prefault_and_lock(created->counts,
                      static_cast<std::size_t>(created->counts_len) * sizeof(std::int64_t));
    return true;
  }

  void record(std::uint64_t ticks) {
    hdr_record_value(histogram.get(), static_cast<std::int64_t>(ticks));
    ++sample_count;
    sum += ticks;
    if (ticks <= kBodyMeanCutoffTicks) {
      ++body_count;
      body_sum += ticks;
    }
  }

  LatencySummary summary() const {
    LatencySummary result;
    result.sample_count = sample_count;
    if (sample_count == 0) return result;
    result.p50 = percentile(50);
    result.p99 = percentile(99);
    result.p999 = percentile(99.9);
    result.max = static_cast<std::uint64_t>(hdr_max(histogram.get()));
    result.mean = static_cast<double>(sum) / static_cast<double>(sample_count);
    if (body_count > 0)
      result.body_mean = static_cast<double>(body_sum) / static_cast<double>(body_count);
    result.share_above_body_cutoff =
        1.0 - static_cast<double>(body_count) / static_cast<double>(sample_count);
    return result;
  }

 private:
  std::uint64_t percentile(double percent) const {
    return static_cast<std::uint64_t>(hdr_value_at_percentile(histogram.get(), percent));
  }
};

// Process counters.

// One reading of this process's cycle, instruction, CPU time and page-fault counters.
// - From proc_pid_rusage V6: public, no root needed, but about 1 us and 8,200 instructions a call
//   (measured on an M1 Pro), so read only around windows and around batches of 1e4 operations
//   or more. A batch's instruction count includes about one call: +0.5 per W-sat publish or
//   dispatched message, about +6 per feed event (1365 events per chunk).
// - Each role is a process with one hot thread, so process totals are that thread's.
// - CPU times are counter ticks on arm64, not ns.
struct ProcessCounters {
  std::uint64_t cycles = 0;
  std::uint64_t p_core_cycles = 0;
  std::uint64_t instructions = 0;
  std::uint64_t cpu_time_ticks = 0;  // user + system time
  std::uint64_t minor_page_faults = 0;
  bool read_ok = false;  // false if a reading failed

  // A reading now; read_ok false if either system call failed.
  static ProcessCounters read() {
    rusage_info_v6 info{};
    struct rusage usage {};
    ProcessCounters counters;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, reinterpret_cast<rusage_info_t*>(&info)) != 0)
      return counters;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return counters;
    counters.cycles = info.ri_cycles;
    counters.p_core_cycles = info.ri_pcycles;
    counters.instructions = info.ri_instructions;
    counters.cpu_time_ticks = info.ri_user_time + info.ri_system_time;
    counters.minor_page_faults = static_cast<std::uint64_t>(usage.ru_minflt);
    counters.read_ok = true;
    return counters;
  }

  // Counts between two readings; read_ok false unless both readings were.
  ProcessCounters operator-(const ProcessCounters& earlier) const {
    ProcessCounters difference;
    if (!read_ok || !earlier.read_ok) return difference;
    difference.cycles = cycles - earlier.cycles;
    difference.p_core_cycles = p_core_cycles - earlier.p_core_cycles;
    difference.instructions = instructions - earlier.instructions;
    difference.cpu_time_ticks = cpu_time_ticks - earlier.cpu_time_ticks;
    difference.minor_page_faults = minor_page_faults - earlier.minor_page_faults;
    difference.read_ok = true;
    return difference;
  }

  // Share of cycles spent on a P-core; the P-core share gate reads it.
  double p_core_share() const {
    if (cycles == 0) return 0;
    return static_cast<double>(p_core_cycles) / static_cast<double>(cycles);
  }

  // Cycles per ns of CPU time: the clock the process actually ran at.
  double effective_ghz() const {
    if (cpu_time_ticks == 0) return 0;
    return static_cast<double>(cycles) / (static_cast<double>(cpu_time_ticks) * kNsPerTick);
  }
};

// Process counters over a bus_bench role's measurement window.
// - start() does nothing once open, so a role calls it on any event it sees inside the window
//   and the first such call reads the counters. Warm-up work stays out of them.
struct CounterWindow {
  ProcessCounters at_start;
  bool is_open = false;

  void start() {
    if (is_open) return;
    at_start = ProcessCounters::read();
    is_open = true;
  }

  ProcessCounters end() const {
    if (!is_open) return ProcessCounters{};
    return ProcessCounters::read() - at_start;
  }
};

// Timed batches.

// Per-operation ns, instructions and cycles of fixed-size batches.
// - Storage is written at construction, so recording never faults.
// - Only batches wholly inside [window_start_tick, window_end_tick) count; the default window
//   takes every batch. Batches past capacity are dropped.
// - Results are lower medians over batches, so one interrupted batch cannot move them.
class TimedBatches {
 public:
  struct BatchSample {
    double ns;
    double instructions;
    double cycles;
  };

 private:
  // One struct per batch, not three arrays of doubles: three arrays change the compiled
  // recording step inside the benchmark loops (checked in the disassembly).
  std::vector<BatchSample> samples;
  std::size_t sample_count = 0;

 public:
  explicit TimedBatches(std::size_t capacity) : samples(capacity) {}

  // Runs body() once as a batch of ops operations and records it if it fell inside the window.
  // Returns the tick it ended at, so the caller has the time without another counter read.
  template <class Body>
  std::uint64_t time_batch(std::uint64_t ops, Body&& body, std::uint64_t window_start_tick = 0,
                           std::uint64_t window_end_tick = UINT64_MAX) {
    const ProcessCounters before = ProcessCounters::read();
    const std::uint64_t start_tick = read_ticks();
    body();
    const std::uint64_t end_tick = read_ticks();
    const ProcessCounters used = ProcessCounters::read() - before;

    const bool inside_window = start_tick >= window_start_tick && end_tick < window_end_tick;
    if (inside_window && sample_count < samples.size()) {
      const double op_count = static_cast<double>(ops);
      BatchSample& sample = samples[sample_count];
      sample.ns = static_cast<double>(end_tick - start_tick) * kNsPerTick / op_count;
      sample.instructions = static_cast<double>(used.instructions) / op_count;
      sample.cycles = static_cast<double>(used.cycles) / op_count;
      ++sample_count;
    }
    return end_tick;
  }

  double median_ns() const {
    return lower_median([](const BatchSample& sample) { return sample.ns; });
  }

  double median_instructions() const {
    return lower_median([](const BatchSample& sample) { return sample.instructions; });
  }

  double median_cycles() const {
    return lower_median([](const BatchSample& sample) { return sample.cycles; });
  }

  bool full() const {
    return sample_count == samples.size();
  }

 private:
  // The lower median of one field (field_of(sample)) over the recorded samples, so it is always
  // one of them. 0 if there are none.
  template <class FieldOf>
  double lower_median(FieldOf field_of) const {
    if (sample_count == 0) return 0;
    std::vector<double> values;
    for (std::size_t i = 0; i < sample_count; ++i) {
      values.push_back(field_of(samples[i]));
    }
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>((values.size() - 1) / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
  }
};

}  // namespace mdbus::bench
