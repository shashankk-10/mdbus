#pragma once

// What a reader does while its next slot is not ready yet: spin, or sleep 1 ms.
// - Consumer<Derived, WaitPolicy> calls idle() once per poll that found nothing, and checks the
//   writer's health every kIdlePollsPerHealthCheck such polls.
// - That count keeps the health check on a time scale: an empty spin poll takes nanoseconds, a
//   sleeping one a millisecond.
// - The writer is not involved either way: it never knows whether anyone waits.
// - spin_pause() is the pause inside short retry loops (the snapshot read, mutant_stress's
//   start barrier); it is not a wait policy.

#include <unistd.h>

#include <cstdint>

namespace mdbus {

// A short pause inside a retry loop.
// - isb makes the core wait for its pipeline to drain: a real pause of tens of cycles.
// - Why not ARM's yield: it only hints that another hardware thread could run, and M1 cores
//   have one thread each.
inline void spin_pause() {
  asm volatile("isb" ::: "memory");
}

// Spin: the lowest wake latency, and it keeps the core. The fast reader's policy (and the
// Consumer default). idle() does nothing: the next poll is the spin.
struct SpinWait {
  // An empty poll is a few ns, so this is a check every few to tens of microseconds; far below
  // the 100 ms heartbeat timeout. Not tuned.
  static constexpr std::uint32_t kIdlePollsPerHealthCheck = 4096;

  void idle() {}
};

constexpr unsigned kSleepMicroseconds = 1000;  // a message waits at most about 1 ms to be seen

// Sleep 1 ms: the slow-reader policy (mdbus_watch). Spinning holds a whole P-core, which a
// viewer has no use for.
struct SleepWait {
  static constexpr std::uint32_t kIdlePollsPerHealthCheck = 1;  // each poll already sleeps 1 ms

  void idle() {
    usleep(kSleepMicroseconds);
  }
};

}  // namespace mdbus
