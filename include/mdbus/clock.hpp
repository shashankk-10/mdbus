#pragma once

// The two clocks. Never mix their values: one tick is 41.67 ns.
// - read_ticks(): the 24 MHz hardware counter, for latency. The Publisher stamps publish_ticks
//   with it and every bench role reads it, so all processes measure on one clock.
// - steady_clock_ns(): std::chrono::steady_clock in nanoseconds, for heartbeats, lock timeouts
//   and probe intervals (the constants.hpp timings are all in these nanoseconds).
// - Why not steady_clock for latency too: it is a library call that converts to ns on every read;
//   the raw counter read is two instructions.

#include <mach/mach_time.h>

#include <chrono>
#include <cstdint>
#include <cstdio>

#include "mdbus/constants.hpp"  // for the arm64-only guard

namespace mdbus {

constexpr std::uint64_t kTicksPerMicrosecond = 24;  // the M1 system counter runs at 24 MHz
constexpr std::uint64_t kTicksPerSecond = 24'000'000;
constexpr double kNsPerTick = 1000.0 / kTicksPerMicrosecond;  // 41.67 ns

constexpr std::uint64_t kNsPerMillisecond = 1'000'000;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
constexpr std::uint64_t kMicrosecondsPerMillisecond = 1000;
constexpr std::uint64_t kNsPerMicrosecond = 1000;

// The counter value now, as the same 24 MHz count in every process.
// - isb: wait until earlier instructions finish, so the counter is not read early, out of
//   order (a reader's tick could then come out before the writer's).
// - mrs cntvct_el0: read the ARM virtual counter (the 24 MHz clock), same value in every
//   process.
inline std::uint64_t read_ticks() {
  std::uint64_t ticks;
  asm volatile("isb\n mrs %0, cntvct_el0" : "=r"(ticks) : : "memory");
  return ticks;
}

// read_ticks(), but only after must_be_computed_first exists.
// - The compiler may move register arithmetic (the checksum) past a plain read_ticks(): the
//   volatile asm stays ordered against memory accesses, not against arithmetic. Passing the
//   checksum in as an input forces it to be computed first.
// - measured: without this the hop grew, all of it checksum arithmetic moved inside the timed
//   window (DESIGN.md, Method, "Three things the method caught", item 3).
inline std::uint64_t read_ticks_after(std::uint64_t must_be_computed_first) {
  std::uint64_t ticks;
  asm volatile("isb\n mrs %0, cntvct_el0" : "=r"(ticks) : "r"(must_be_computed_first) : "memory");
  return ticks;
}

// Nanoseconds to counter ticks in integers, rounding down; used for thresholds set in ns.
// - Reports go the other way (ticks to ns) with kNsPerTick, as a double, at the very end.
// - Multiplies before dividing, so a few hundred ns does not round to 0 first.
// Example:
//   ns_to_ticks(667)        -> 16      (667 x 24 / 1000 = 16.008)
//   ns_to_ticks(1'000'000)  -> 24000   (1 ms)
//   ns_to_ticks(41)         -> 0       (under one 41.67 ns tick)
constexpr std::uint64_t ns_to_ticks(std::uint64_t ns) {
  return ns * kTicksPerMicrosecond / kNsPerMicrosecond;
}

// Nanoseconds of std::chrono::steady_clock. One clock for every process on the machine, so a
// reader can compare its own reading with the writer's heartbeat.
inline std::uint64_t steady_clock_ns() {
  const auto since_start = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since_start).count();
  return static_cast<std::uint64_t>(ns);
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

}  // namespace mdbus
