#pragma once

// bus_bench's role processes, used by the launcher in bench/bus_bench.cpp.
// - spawn() starts this same executable again with --role and --name added.
// - wait_until_all_ready / wait_until_all_done poll the roles' states in the control segment.
// - reap() collects one role's exit code, killing it if it hangs.

#include <pthread/qos.h>
#include <pthread/spawn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bench_args.hpp"
#include "bus_bench_control.hpp"
#include "timing.hpp"

extern char** environ;

namespace mdbus::bench {

constexpr unsigned kWaitPollMicroseconds = 500;   // launcher's poll of the role states
constexpr unsigned kReapPollMicroseconds = 2000;  // launcher's poll for a role's exit
constexpr std::uint64_t kReapTimeoutMs = 5'000;   // then SIGKILL
// A process killed by signal N reports exit code kExitCodeSignalBase + N, as a shell does.
constexpr int kExitCodeSignalBase = 128;

// Starts this executable again as one role. Returns its pid, or -1.
// - argv[0] is the path the launcher was started by (ctest, compare.py and a shell all give
//   one), and the working directory does not change.
// - An E-core role starts at BACKGROUND QoS, set through the spawn attributes, which can only
//   set UTILITY and BACKGROUND. A P-core role raises itself to USER_INTERACTIVE once running.
inline pid_t spawn(const BenchArgs& args, Role role, const std::string& name, bool e_core) {
  std::vector<std::string> child_args(args.argv, args.argv + args.argc);
  child_args.push_back("--role");
  child_args.push_back(kRoleNames[role]);
  child_args.push_back("--name");
  child_args.push_back(name);

  std::vector<char*> argv;
  for (std::string& arg : child_args) argv.push_back(arg.data());
  argv.push_back(nullptr);

  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  if (e_core) posix_spawnattr_set_qos_class_np(&attributes, QOS_CLASS_BACKGROUND);
  pid_t pid = -1;
  const int error = posix_spawn(&pid, argv[0], nullptr, &attributes, argv.data(), environ);
  posix_spawnattr_destroy(&attributes);
  if (error != 0) return -1;
  return pid;
}

// One spawned role, as the launcher tracks it.
struct RoleProcess {
  Role role;
  pid_t pid;  // -1 if it never started or was already reaped
};

// Whether every role in role_processes has reported state.
inline bool all_roles_in_state(const BenchControl& control,
                               const std::vector<RoleProcess>& role_processes, RoleState state) {
  for (const RoleProcess& process : role_processes) {
    const RoleResults& results = control.role_results[process.role];
    if (results.state.load(std::memory_order_acquire) != state) return false;
  }
  return true;
}

// Waits until every role reports kReady. False if a role exits first (aborted = "died:" + its
// name) or timeout_ms passes (aborted = "not_ready").
inline bool wait_until_all_ready(const BenchControl& control,
                                 std::vector<RoleProcess>& role_processes, std::uint64_t timeout_ms,
                                 std::string& aborted) {
  const std::uint64_t deadline = read_ticks() + ms_to_ticks(timeout_ms);
  while (read_ticks() < deadline) {
    for (RoleProcess& process : role_processes) {
      int status = 0;
      if (process.pid > 0 && waitpid(process.pid, &status, WNOHANG) == process.pid) {
        process.pid = -1;
        aborted = std::string("died:") + kRoleNames[process.role];
        return false;
      }
    }
    if (all_roles_in_state(control, role_processes, kReady)) return true;
    usleep(kWaitPollMicroseconds);
  }
  aborted = "not_ready";
  return false;
}

// Waits up to timeout_ms until every role reports kDone. A role that is still not done is
// caught when it is reaped.
inline void wait_until_all_done(const BenchControl& control,
                                const std::vector<RoleProcess>& role_processes,
                                std::uint64_t timeout_ms) {
  const std::uint64_t deadline = read_ticks() + ms_to_ticks(timeout_ms);
  while (read_ticks() < deadline) {
    if (all_roles_in_state(control, role_processes, kDone)) return;
    usleep(kWaitPollMicroseconds);
  }
}

// Waits up to kReapTimeoutMs for a role to exit, then kills it. Returns its exit status,
// kExitCodeSignalBase + the signal that ended it, or -1 if it never started or was already
// reaped.
inline int reap(pid_t pid) {
  if (pid <= 0) return -1;
  int status = 0;
  const std::uint64_t deadline = read_ticks() + ms_to_ticks(kReapTimeoutMs);

  pid_t exited = waitpid(pid, &status, WNOHANG);
  while (exited == 0 && read_ticks() < deadline) {
    usleep(kReapPollMicroseconds);
    exited = waitpid(pid, &status, WNOHANG);
  }
  if (exited == 0) {
    ::kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }

  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return kExitCodeSignalBase + WTERMSIG(status);
}

}  // namespace mdbus::bench
