#pragma once

// bus_bench's role processes, as its launcher starts and tracks them.
// - spawn() starts this same executable again with --role and --name added.
// - wait_until_all() polls the roles' states in the control segment.
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
// - argv[0] is what the launcher was started as: a path (ctest, compare.py), or a bare name
//   that posix_spawnp finds on PATH as the shell did. The working directory does not change.
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
  const int error = posix_spawnp(&pid, argv[0], nullptr, &attributes, argv.data(), environ);
  posix_spawnattr_destroy(&attributes);
  if (error != 0) return -1;
  return pid;
}

// One spawned role, as the launcher tracks it.
struct RoleProcess {
  Role role;
  pid_t pid;  // -1 if it never started
};

// Whether the process has exited, without reaping it: reap() still collects its exit code.
inline bool has_exited(pid_t pid) {
  siginfo_t info{};
  const int options = WEXITED | WNOHANG | WNOWAIT;
  return pid > 0 && waitid(P_PID, static_cast<id_t>(pid), &info, options) == 0 &&
         info.si_pid == pid;
}

// Waits until every role reports state. False if a role exits without reporting it (aborted =
// "died:<role>") or timeout_ms passes (aborted = "not_<state>:<the first role not there>").
inline bool wait_until_all(const BenchControl& control,
                           const std::vector<RoleProcess>& role_processes, RoleState state,
                           std::uint64_t timeout_ms, std::string& aborted) {
  const std::uint64_t deadline = read_ticks() + ms_to_ticks(timeout_ms);
  while (true) {
    const RoleProcess* not_there = nullptr;
    for (const RoleProcess& process : role_processes) {
      const auto reported = [&] {
        return control.role_results[process.role].state.load(std::memory_order_acquire) == state;
      };
      if (reported()) continue;
      // A role stores its state before it exits, so a state read after its exit is final.
      if (has_exited(process.pid) && !reported()) {
        aborted = std::string("died:") + kRoleNames[process.role];
        return false;
      }
      if (not_there == nullptr) not_there = &process;
    }
    if (not_there == nullptr) return true;
    if (read_ticks() >= deadline) {
      aborted = std::string("not_") + kRoleStateNames[state] + ":" + kRoleNames[not_there->role];
      return false;
    }
    usleep(kWaitPollMicroseconds);
  }
}

// Waits up to kReapTimeoutMs for a role to exit, then kills it. Returns its exit status,
// kExitCodeSignalBase + the signal that ended it, or -1 if it never started.
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
