#pragma once

// Tick conversions, the counter check and the writer's open-loop schedule for the benchmarks.

#include <mach/mach_time.h>

#include <cstdint>
#include <cstdio>

#include "mdbus/clock.hpp"

namespace mdbus::bench {

constexpr std::uint64_t kTicksPerSecond = 24'000'000;
constexpr std::uint64_t kMillisecondsPerSecond = 1000;

// Nanoseconds to counter ticks in integers, rounding down; used for thresholds set in ns.
// - Reports go the other way (ticks to ns) with kNsPerTick, as a double, at the very end.
// - Multiplies before dividing, so a few hundred ns does not round to 0 first.
constexpr std::uint64_t ns_to_ticks(std::uint64_t ns) {
  return ns * kTicksPerMicrosecond / kNsPerMicrosecond;
}

constexpr std::uint64_t ms_to_ticks(std::uint64_t ms) {
  return ms * (kTicksPerSecond / kMillisecondsPerSecond);
}

// Busy-waits (no sleep, no yield) until the counter reaches target_tick.
inline void spin_until_tick(std::uint64_t target_tick) {
  while (read_ticks() < target_tick) {}
}

// The open-loop schedule: message message_index is due at go_tick + message_index *
// period_ticks, rounded down, whatever happened to the messages before it.
// - A writer that falls behind publishes its backlog late, and counts it, instead of quietly
//   lowering the rate. Computed from the index, so no rounding error accumulates.
// - The writer spins to the due tick, so every publish starts just after a counter edge and a
//   latency read in whole ticks rounds the same way every time.
// - The cost: a paced mean is a smoothed staircase in the true latency. A pure shift of D ticks
//   reads as anything from floor(D) to floor(D) + 1 ticks, depending on the hop's spread, so a
//   sub-tick A/B difference is not resolved (bench/decisions.yaml).
// Example (go_tick 1000, period_ticks 2.5, which is 9.6 M messages/s):
//   message_index 0 -> 1000, 1 -> 1002, 2 -> 1005, 3 -> 1007
inline std::uint64_t due_tick(std::uint64_t go_tick, double period_ticks,
                              std::uint64_t message_index) {
  return go_tick + static_cast<std::uint64_t>(static_cast<double>(message_index) * period_ticks);
}

// True when the counter is the 24 MHz one that every tick conversion in the repo assumes.
// - macOS reports the counter as the ratio 125/3 ns per tick through mach_timebase_info.
// - On any other ratio it prints both to stderr and returns false; bus_bench then fails its
//   "clock" gate.
inline bool verify_counter_is_24mhz() {
  const std::uint32_t expected_numer = 125;
  const std::uint32_t expected_denom = 3;
  mach_timebase_info_data_t timebase{};
  mach_timebase_info(&timebase);
  if (timebase.numer == expected_numer && timebase.denom == expected_denom) return true;
  std::fprintf(stderr, "mdbus: timebase %u/%u, expected %u/%u (the 24 MHz counter)\n",
               timebase.numer, timebase.denom, expected_numer, expected_denom);
  return false;
}

}  // namespace mdbus::bench
