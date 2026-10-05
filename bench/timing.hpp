#pragma once

// Tick conversions and the writer's open-loop schedule for the benchmarks.
// - Used by bus_bench (writer pacing, warmup and window ticks), child_processes.hpp (timeouts)
//   and dispatch_bench (run length).
// - A tick is one count of the 24 MHz counter read_ticks() reads (clock.hpp).

#include <cstdint>

#include "mdbus/clock.hpp"

namespace mdbus::bench {

constexpr std::uint64_t kMillisecondsPerSecond = 1000;

// Example: ms_to_ticks(1) == 24'000, ms_to_ticks(250) == 6'000'000.
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
// - The cost: a mean latency in ticks is a staircase in the true latency, so an A/B difference
//   smaller than a tick reads as 0 or as a whole tick. Paced means resolve whole ticks only.
// Example (go_tick 1000, period_ticks 2.5, which is 9.6 M messages/s):
//   message_index 0 -> 1000, 1 -> 1002, 2 -> 1005, 3 -> 1007
inline std::uint64_t due_tick(std::uint64_t go_tick, double period_ticks,
                              std::uint64_t message_index) {
  return go_tick + static_cast<std::uint64_t>(static_cast<double>(message_index) * period_ticks);
}

}  // namespace mdbus::bench
