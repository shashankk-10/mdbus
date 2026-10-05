#pragma once

// Two jobs, both about the writer of one bus:
// - WriterLock: at most one writer per bus. The writer holds an exclusive flock on the bus's
//   lock file for as long as it runs; BusWriter takes it before touching the segment.
// - WriterHealthMonitor: a reader's verdict on its writer (Alive / Stalled / Down), used by
//   Consumer only while the ring is quiet. A heartbeat answers quickly; the flock answers for
//   sure.
// - flock: an advisory lock on an open file, which the kernel drops when the holder dies.
// - heartbeat: a steady-clock time the writer stores in the control block at least every 1 ms,
//   even when it has nothing to publish.

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>

#include "mdbus/bus_paths.hpp"
#include "mdbus/clock.hpp"
#include "mdbus/control_block.hpp"
#include "mdbus/status.hpp"

namespace mdbus {

// A reader's judgement of the writer, kept in the reader's own memory.
// - Not the same as WriterState (control_block.hpp): that is what the writer stores about
//   itself in the segment. WriterHealth is worked out from that word, the heartbeat and the
//   flock, and covers the case WriterState cannot: a writer that died without saying so.
// - Stalled is never reported as Down: a stopped writer may resume and keep its bus.
enum class WriterHealth : std::uint8_t {
  Alive,    // heartbeat fresh
  Stalled,  // heartbeat old, flock still held: stopped or descheduled, not dead
  Down,     // flock free (the kernel released it), or the writer exited or was replaced
};

// The flock on a bus's lock file: held exclusively by the writer, probed by readers.
// - The kernel drops a flock when its holder dies, SIGKILL included, but keeps it while the
//   holder is stopped (SIGSTOP). So the lock tells dead from stalled.
// - Why a flock and not a pid file: a pid can be reused by an unrelated process after the
//   writer dies; a lock cannot outlive its holder.
class WriterLock {
 private:
  // A probe holds a shared lock for a few microseconds, so 200 us between attempts gives
  // about 50 attempts inside kWriterLockTimeoutNs (10 ms).
  static constexpr unsigned kLockRetrySleepMicroseconds = 200;
  static constexpr mode_t kOwnerOnlyFileMode = 0600;       // read and write for this user only
  static constexpr mode_t kOwnerOnlyDirectoryMode = 0700;  // only this user can add or remove

  int lock_fd = -1;

 public:
  WriterLock() = default;
  WriterLock(const WriterLock&) = delete;
  WriterLock& operator=(const WriterLock&) = delete;
  ~WriterLock() {
    close();
  }

  // Takes the exclusive lock for the writer, retrying for up to timeout_ns.
  // - Returns Ok, AnotherWriterRunning (still held at the deadline) or SystemCallFailed.
  // - Why retry: a reader's probe holds a shared lock for a few microseconds, so one failed
  //   attempt does not prove another writer exists.
  Status acquire_exclusive(const BusPaths& paths, std::uint64_t timeout_ns) {
    const std::uint64_t deadline_ns = steady_clock_ns() + timeout_ns;
    if (open_lock_file(paths) != Status::Ok) return Status::SystemCallFailed;
    while (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
      const bool busy = errno == EWOULDBLOCK || errno == EINTR;
      if (!busy) {
        close();
        return Status::SystemCallFailed;
      }
      if (steady_clock_ns() >= deadline_ns) {
        close();
        return Status::AnotherWriterRunning;
      }
      usleep(kLockRetrySleepMicroseconds);
    }
    return Status::Ok;
  }

  // Opens (and if needed creates) the lock file without locking it: the writer locks it next,
  // a reader only probes it.
  // - Creating it from a reader does no harm: the writer opens the same path, and nobody
  //   unlinks it while the bus is in use.
  // - The directory is private (0700), so no other user can remove or replace a lock file in
  //   it. It already existing (EEXIST) is fine.
  // - O_CLOEXEC, so a spawned child cannot inherit the fd and keep a dead writer's lock alive.
  Status open_lock_file(const BusPaths& paths) {
    close();
    if (mkdir(paths.lock_directory.c_str(), kOwnerOnlyDirectoryMode) != 0 && errno != EEXIST)
      return Status::SystemCallFailed;
    lock_fd = ::open(paths.lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, kOwnerOnlyFileMode);
    if (lock_fd < 0) return Status::SystemCallFailed;
    return Status::Ok;
  }

  // True if some other open of the lock file holds it: a running writer, or a stopped one.
  // - Tries a shared lock without waiting. Success means nobody holds it; the shared lock is
  //   dropped at once, so a restarting writer is not blocked by the probe.
  bool held_by_another_process() const {
    if (lock_fd < 0) return false;
    if (flock(lock_fd, LOCK_SH | LOCK_NB) == 0) {
      flock(lock_fd, LOCK_UN);
      return false;
    }
    return errno == EWOULDBLOCK;
  }

  void close() {
    if (lock_fd >= 0) ::close(lock_fd);  // closing the last fd releases the lock
    lock_fd = -1;
  }
};

// A reader's watch on its writer, run from the idle path only.
// - A message arriving proves the writer alive, so a busy reader never probes.
// - Once the reader has been quiet for heartbeat_timeout_ns (100 ms by default), it probes the
//   writer, at most once per kWriterProbeIntervalNs (1 ms).
// - Without a lock file (an in-process bus) an old heartbeat reads as Down.
class WriterHealthMonitor {
 private:
  const ControlBlock<StdAtomics>* control = nullptr;
  WriterLock probe_lock;            // opened only to probe; this reader never holds it
  std::uint64_t idle_since_ns = 0;  // 0 until the first idle poll after a message
  std::uint64_t last_probe_ns = 0;
  std::uint64_t heartbeat_timeout_ns = kHeartbeatTimeoutNs;

 public:
  // Starts watching the writer of a newly mapped segment. paths is null for an in-process bus.
  void start_watching(const ControlBlock<StdAtomics>* control_block, const BusPaths* paths) {
    control = control_block;
    probe_lock.close();
    if (paths != nullptr && probe_lock.open_lock_file(*paths) != Status::Ok) probe_lock.close();
    idle_since_ns = 0;
    last_probe_ns = 0;
  }

  void set_heartbeat_timeout(std::uint64_t timeout_ns) {
    heartbeat_timeout_ns = timeout_ns;
  }

  void note_message_received() {
    idle_since_ns = 0;
  }

  // Called on an idle poll: probes if the reader has been quiet too long and has not probed in
  // the last 1 ms, and returns `current` unchanged otherwise.
  // - The first idle call after a message only starts the quiet clock.
  WriterHealth check_if_due(WriterHealth current) {
    const std::uint64_t now_ns = steady_clock_ns();
    if (idle_since_ns == 0) idle_since_ns = now_ns;
    const bool quiet_too_long = now_ns - idle_since_ns > heartbeat_timeout_ns;
    const bool probed_recently = now_ns - last_probe_ns < kWriterProbeIntervalNs;
    if (!quiet_too_long || probed_recently) return current;
    last_probe_ns = now_ns;
    return probe_now(now_ns);
  }

  // One probe at now_ns, keeping no state. Cheapest evidence first:
  // 1. The writer's own state word: Exited or Replaced means Down.
  // 2. The heartbeat's age: within the timeout means Alive.
  // 3. Only then the flock, which costs system calls: held means Stalled, free means Down.
  // Example (heartbeat timeout 100 ms, writer state Running):
  //   heartbeat 50 ms old                       -> Alive
  //   heartbeat 150 ms old, lock still held     -> Stalled (writer stopped with SIGSTOP)
  //   heartbeat 150 ms old, lock free           -> Down (writer died)
  //   heartbeat 1 ns newer than now_ns          -> Alive
  //   writer state Exited, heartbeat 1 ms old   -> Down
  WriterHealth probe_now(std::uint64_t now_ns) const {
    // 1. The state word.
    const WriterState writer_state = load_writer_state(*control);
    if (writer_state == WriterState::Replaced || writer_state == WriterState::Exited)
      return WriterHealth::Down;

    // 2. heartbeat_ns > now_ns: the writer stored it after this reader read the clock, and
    //    now_ns - heartbeat_ns would wrap.
    const std::uint64_t heartbeat_ns = StdAtomics::load_acquire(control->liveness.heartbeat_ns);
    if (heartbeat_ns >= now_ns || now_ns - heartbeat_ns <= heartbeat_timeout_ns)
      return WriterHealth::Alive;

    // 3. The flock.
    if (probe_lock.held_by_another_process()) return WriterHealth::Stalled;
    return WriterHealth::Down;
  }
};

}  // namespace mdbus
