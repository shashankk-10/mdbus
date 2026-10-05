#pragma once

// A small test harness. TEST cases register themselves; CHECK records a failure and goes on,
// REQUIRE leaves the case. CHILD_PROCESS entry points let a test re-run its own binary as another
// process (spawn_self), which is how cross-process cases get real processes to kill and stop.
// The shared main() in test_main.cpp calls run_main().

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mdbus/bus_paths.hpp"
#include "mdbus/clock.hpp"

// Insurance: any assert() added later must stay live in tests.
#if defined(NDEBUG)
#error "tests must build with -UNDEBUG"
#endif

extern char** environ;

namespace mdbus_test {

struct TestEntry {
  const char* name;
  void (*test)();
  int (*child)(int, char**);
};

// Each TEST and CHILD_PROCESS macro defines a static TestRegistration object whose constructor
// adds the case to entries() before main runs. The lists are function-local statics, so they
// exist before the first registration whatever order the static objects are constructed in.
inline std::vector<TestEntry>& entries() {
  static std::vector<TestEntry> list;
  return list;
}

inline std::vector<std::string>& buses() {
  static std::vector<std::string> list;
  return list;
}

inline int& failures() {
  static int count = 0;
  return count;
}

inline std::string& self_path() {
  static std::string path;
  return path;
}

struct TestRegistration {
  explicit TestRegistration(TestEntry entry) {
    entries().push_back(entry);
  }
};

inline void fail(const char* file, int line, const char* what) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what);
}

// Starts the program args[0] with the arguments that follow it, and its stdout into the file
// stdout_path if one is named. -1 if it could not start.
inline pid_t spawn_program(std::vector<std::string> args, const std::string& stdout_path = "") {
  std::vector<char*> argv;
  for (std::string& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (!stdout_path.empty())
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
  pid_t pid = -1;
  const int failed = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  return failed == 0 ? pid : -1;
}

// Starts this test binary again (argv[0] as ctest ran it) as the CHILD_PROCESS child_name, with
// args.
inline pid_t spawn_self(const char* child_name, std::vector<std::string> args = {}) {
  args.insert(args.begin(), {self_path(), "--child", child_name});
  return spawn_program(std::move(args));
}

// A child killed by signal N reports exit code kExitCodeSignalBase + N, as a shell does.
constexpr int kExitCodeSignalBase = 128;

// Kills a spawned child on every exit path, so a failed REQUIRE never leaves a stopped process
// holding a lock.
struct ChildProcess {
  pid_t pid;

  explicit ChildProcess(pid_t p) : pid(p) {}
  ChildProcess(const ChildProcess&) = delete;
  ~ChildProcess() {
    kill_and_wait();
  }

  int kill_and_wait() {
    if (pid > 0) {
      ::kill(pid, SIGCONT);
      ::kill(pid, SIGKILL);
    }
    return wait_for_exit_code();
  }

  int wait_for_exit_code() {
    if (pid <= 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    pid = -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return kExitCodeSignalBase + WTERMSIG(status);
    return -1;
  }
};

// Unique per process, so parallel ctest runs never share a bus; removed after the last case.
inline std::string make_test_bus_name(const std::string& tag) {
  const std::string name = "t" + tag + std::to_string(getpid());
  buses().push_back(name);
  return name;
}

inline void sleep_ms(unsigned ms) {
  usleep(static_cast<useconds_t>(ms * mdbus::kMicrosecondsPerMillisecond));
}

// Runs the CHILD_PROCESS child_name with the arguments after it; 1 if there is none.
inline int run_child(const char* child_name, int argc, char** argv) {
  for (const TestEntry& e : entries()) {
    if (e.child != nullptr && std::strcmp(e.name, child_name) == 0) return e.child(argc, argv);
  }
  return 1;
}

// Runs every TEST, or only the one named filter when filter is not null. 0 if at least one ran
// and none failed.
inline int run_tests(const char* filter) {
  int ran = 0;
  for (const TestEntry& e : entries()) {
    if (e.test == nullptr) continue;
    if (filter != nullptr && std::strcmp(filter, e.name) != 0) continue;
    const int before = failures();
    std::fprintf(stderr, "[ RUN  ] %s\n", e.name);
    e.test();
    std::fprintf(stderr, "[ %s ] %s\n", failures() == before ? " OK " : "FAIL", e.name);
    ++ran;
  }

  for (const std::string& b : buses()) mdbus::destroy_bus(b);
  std::fprintf(stderr, "%d case(s), %d failure(s)\n", ran, failures());
  if (ran > 0 && failures() == 0) return 0;
  return 1;
}

// test_binary --child NAME ARGS...: runs one child. test_binary [TEST]: runs the tests.
inline int run_main(int argc, char** argv) {
  self_path() = argv[0];
  if (argc >= 3 && std::strcmp(argv[1], "--child") == 0) {
    return run_child(argv[2], argc - 3, argv + 3);
  }
  const char* filter = nullptr;
  if (argc >= 2) filter = argv[1];
  return run_tests(filter);
}

}  // namespace mdbus_test

#define TEST(name) \
  static void name(); \
  static ::mdbus_test::TestRegistration reg_##name({#name, &name, nullptr}); \
  static void name()

#define CHILD_PROCESS(name) \
  static int name(int argc, char** argv); \
  static ::mdbus_test::TestRegistration reg_##name({#name, nullptr, &name}); \
  static int name([[maybe_unused]] int argc, [[maybe_unused]] char** argv)

#define CHECK(c) \
  do { \
    if (!(c)) ::mdbus_test::fail(__FILE__, __LINE__, #c); \
  } while (0)

#define REQUIRE(c) \
  do { \
    if (!(c)) { \
      ::mdbus_test::fail(__FILE__, __LINE__, #c); \
      return; \
    } \
  } while (0)
