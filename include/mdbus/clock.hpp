#pragma once

// The two clocks. Never mix their values: one tick is 41.67 ns.
// - read_ticks(): the 24 MHz hardware counter, for latency. The Publisher stamps publish_ticks
//   with it and every bench role reads it, so all processes measure on one clock.
// - steady_clock_ns(): std::chrono::steady_clock in nanoseconds, for heartbeats, lock timeouts
//   and probe intervals (the constants.hpp timings are all in these nanoseconds).
// - Why not steady_clock for latency too: it is a library call that converts to ns on every read;
//   the raw counter read is two instructions.

#include <chrono>
#include <cstdint>

#include "mdbus/constants.hpp"  // for the arm64-only guard

namespace mdbus {

constexpr std::uint64_t kTicksPerMicrosecond = 24;  // the M1 system counter runs at 24 MHz
constexpr double kNsPerTick = 1000.0 / kTicksPerMicrosecond;  // 41.67 ns

constexpr std::uint64_t kNsPerMillisecond = 1'000'000;
constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
constexpr std::uint64_t kMicrosecondsPerMillisecond = 1000;
constexpr std::uint64_t kNsPerMicrosecond = 1000;

// The counter value now, as the same 24 MHz count in every process.
// - isb: wait until earlier instructions finish, so the counter is not read early, out of
//   order (a reader's tick could then come out before the writer's).
// - mrs cntvct_el0: read the ARM virtual counter.
inline std::uint64_t read_ticks() {
  std::uint64_t ticks;
  asm volatile("isb\n mrs %0, cntvct_el0" : "=r"(ticks) : : "memory");
  return ticks;
}

// read_ticks(), but only after must_be_computed_first exists.
// - The compiler may move register arithmetic (the checksum) past a plain read_ticks(): the
//   volatile asm stays ordered against memory accesses, not against arithmetic. Passing the
//   checksum in as an input forces it to be computed first.
// - Seen in the disassembly (Apple clang 15): with a plain read_ticks() in write_publish_ticks,
//   the checksum's last multiply and xor are scheduled after the counter read, inside the timed
//   window.
inline std::uint64_t read_ticks_after(std::uint64_t must_be_computed_first) {
  std::uint64_t ticks;
  asm volatile("isb\n mrs %0, cntvct_el0" : "=r"(ticks) : "r"(must_be_computed_first) : "memory");
  return ticks;
}

// Nanoseconds of std::chrono::steady_clock. One clock for every process on the machine, so a
// reader can compare its own reading with the writer's heartbeat.
inline std::uint64_t steady_clock_ns() {
  const auto since_start = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since_start).count();
  return static_cast<std::uint64_t>(ns);
}

}  // namespace mdbus
