#pragma once

// Two jobs, both about the writer of one bus:
// - WriterLock: at most one writer per bus. The writer holds an exclusive flock on the bus's
//   lock file for as long as it runs; BusWriter takes it before touching the segment.
// - judge_writer: a reader's verdict on its writer, Alive or Down, from two words the writer
//   keeps in the control block (control_block.hpp): its state (Exited on a clean shutdown) and
//   its heartbeat.
// - flock: an advisory lock on an open file. The kernel drops it when the holder dies,
//   SIGKILL included, so a crashed writer never blocks its successor.
// - heartbeat: a steady-clock time the writer stores at most once per 1 ms: RingWriter::publish
//   checks every 1024 publishes, and the writer's loop calls heartbeat_if_due when idle or busy
//   without publishing (the feed handler: every 64 packets).

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>

#include "mdbus/bus_paths.hpp"
#include "mdbus/control_block.hpp"
#include "mdbus/status.hpp"

namespace mdbus {

// A reader's judgement of the writer, kept in the reader's own memory.
// - Down: the writer stored Exited, or nothing (no message, no heartbeat) for the timeout. A
//   writer stopped with SIGSTOP also reads Down, and Alive again at its next message.
enum class WriterHealth : std::uint8_t { Alive, Down };

// The value's name, for printing.
inline const char* health_name(WriterHealth health) {
  return health == WriterHealth::Alive ? "alive" : "down";
}

// The flock on a bus's lock file, held exclusively by the writer.
// - Why a flock and not a pid file: a pid can be reused by an unrelated process after the
//   writer dies; a lock cannot outlive its holder.
class WriterLock {
 private:
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

  // Opens (creating if needed) the bus's lock file and takes the exclusive lock, without
  // waiting: Ok, AnotherWriterRunning, or SystemCallFailed (errno says why).
  // - O_CLOEXEC, so a spawned child cannot inherit the fd and keep a dead writer's lock alive.
  Status acquire(const BusPaths& paths) {
    close();
    if (mkdir(paths.lock_directory.c_str(), kOwnerOnlyDirectoryMode) != 0 && errno != EEXIST)
      return Status::SystemCallFailed;
    lock_fd = ::open(paths.lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, kOwnerOnlyFileMode);
    if (lock_fd < 0) return Status::SystemCallFailed;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0) return Status::Ok;
    const bool held = errno == EWOULDBLOCK;
    close();
    return held ? Status::AnotherWriterRunning : Status::SystemCallFailed;
  }

  void close() {
    if (lock_fd >= 0) ::close(lock_fd);  // closing the last fd releases the lock
    lock_fd = -1;
  }
};

// The reader's verdict at now_ns, after quiet_ns without a message.
// - Exited is final for a segment, so it is Down at once, however recent the last message.
// - Otherwise a message within the timeout, or a heartbeat within it, means Alive.
// - heartbeat_ns > now_ns: the writer stored it after this reader read the clock.
// Example (timeout 100 ms):
//   state Exited                                 -> Down
//   quiet 50 ms                                  -> Alive
//   quiet 150 ms, heartbeat 1 ms old             -> Alive (an idle writer)
//   quiet 150 ms, heartbeat 150 ms old           -> Down (dead, or stopped)
inline WriterHealth judge_writer(const ControlBlock<StdAtomics>& control, std::uint64_t now_ns,
                                 std::uint64_t quiet_ns, std::uint64_t timeout_ns) {
  if (load_writer_state(control) == WriterState::Exited) return WriterHealth::Down;
  if (quiet_ns <= timeout_ns) return WriterHealth::Alive;
  const std::uint64_t heartbeat_ns = StdAtomics::load_acquire(control.liveness.heartbeat_ns);
  if (heartbeat_ns >= now_ns || now_ns - heartbeat_ns <= timeout_ns) return WriterHealth::Alive;
  return WriterHealth::Down;
}

}  // namespace mdbus
