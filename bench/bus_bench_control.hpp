#pragma once

// bus_bench's control segment, one shm mapping shared by the launcher and every role.
// - The launcher writes LauncherCommand; the bus's writer writes BusProgress; each role writes
//   its own RoleResults.
// - Every part starts on its own 128 B line, so no two processes write the same line.

#include <atomic>
#include <cstdint>

#include "measurement.hpp"
#include "mdbus/constants.hpp"

namespace mdbus::bench {

// The roles, as indexes into BenchControl::role_results. kRoleNames are the --role values and
// the column suffixes in row.csv (ghz_writer, pshare_fast, ...).
enum Role : unsigned { kWriter, kFastReader, kSlowReader, kRoleCount };
constexpr const char* kRoleNames[kRoleCount] = {"writer", "fast", "slow"};

// The control segment's shm name is kShmNamePrefix + the bench bus name + this suffix.
constexpr const char* kBenchControlSuffix = ".c";

// A role's progress, in RoleResults::state.
enum RoleState : std::uint64_t { kStarting, kReady, kDone };
constexpr const char* kRoleStateNames[] = {"starting", "ready", "done"};

// One role's measurements. The role fills them in, then stores state = kDone with release, so
// the launcher's acquire load of kDone sees every field.
struct alignas(kCacheLineBytes) RoleResults {
  std::atomic<std::uint64_t> state;
  std::uint64_t messages_read;
  std::uint64_t latency_samples;
  std::uint64_t times_lapped;
  std::uint64_t messages_lost;
  std::uint64_t causality_violations;
  std::uint64_t messages_published;
  std::uint64_t late_publishes;
  std::uint64_t snapshot_reads;
  std::uint64_t snapshot_read_retries;
  std::uint64_t snapshot_reads_gave_up;
  double achieved_rate;
  double wsat_ns_per_publish;  // W-sat medians per publish
  double wsat_instructions_per_publish;
  double wsat_cycles_per_publish;
  LatencySummary hop;
  LatencySummary e2e;
  LatencySummary snapshot_read_latency;
  ProcessCounters process_counters;
};

// The bus's progress, stored by its writer.
struct alignas(kCacheLineBytes) BusProgress {
  std::atomic<std::uint64_t> first_seq;    // readers start here; stored before ready
  std::atomic<std::uint64_t> final_seq;    // one past the last message; valid once writer_done
  std::atomic<std::uint64_t> writer_done;  // 1, with release, after final_seq
};

// The launcher's commands. The ticks are written before go is stored with release, so a role
// that has seen go reads them with plain loads.
struct alignas(kCacheLineBytes) LauncherCommand {
  std::atomic<std::uint64_t> go;
  std::atomic<std::uint64_t> stop;
  std::uint64_t go_tick;            // the paced schedule's tick 0
  std::uint64_t window_start_tick;  // go_tick + warm-up
  std::uint64_t window_end_tick;    // window_start_tick + duration
};

// The whole control segment. The launcher constructs it with placement new; roles cast.
struct BenchControl {
  LauncherCommand command;
  BusProgress hot_bus_progress;
  RoleResults role_results[kRoleCount];
};

}  // namespace mdbus::bench
