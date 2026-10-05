#pragma once

// The first thing in every bus segment: the ready marker, the writer's state, and the head hint.
// - Built by SegmentFormat::construct(), checked by SegmentFormat::validate() when a reader
//   attaches.
// - RingWriter stores the heartbeat and the head hint; Publisher and BusWriter store the writer
//   state; WriterHealthMonitor and Consumer read them.
// - head hint: the next seq to publish, stored every 64 publishes, so it trails by up to 63.
// - false sharing: two cores writing different data in the same cache line.

#include <cstddef>
#include <cstdint>

#include "mdbus/atomic_policy.hpp"
#include "mdbus/constants.hpp"

namespace mdbus {

// The segment's "ready" marker. In memory (little-endian) its bytes are the ASCII text
// "MDBUSRDY", so `hexdump -C` of the segment shows the word at offset 0.
// - The writer stores it last, with release, after building the control block, the ring and the
//   snapshot table. A reader that loads it with acquire therefore sees a complete segment.
// - A reader that loads 0 knows the writer is still building it (Status::NotReady).
// - A reader that loads any other value knows this shm object is not an mdbus segment
//   (Status::NotABusSegment).
// - It is not a version number; kLayoutVersion and the layout hash do that job.
constexpr std::uint64_t kSegmentReadyMarker =
    0x5944'5253'5542'444d;  // bytes 4d 44 42 55 53 52 44 59

// What the segment's writer is doing, as readers see it.
// - NotStarted: 0, a fresh segment.
// - Running: set by Publisher::mark_running.
// - Exited: clean shutdown.
// - Replaced: a newer writer took the bus; readers re-attach by name.
// - Stored by the segment's own writer (Running, Exited) and by a newer writer (Replaced). The
//   newer one exists only once it holds the lock, so after the old writer died or closed: the
//   two never store at the same time.
enum class WriterState : std::uint64_t { NotStarted, Running, Exited, Replaced };

// The 384 B block at offset 0 of every bus segment.
// - Three parts, each on its own 128 B line, because each is written at a different rate:
//   identity once, liveness every 1 ms, the head hint every 64 publishes. A store to one part
//   never takes away the line holding another (false sharing).
// - Written by this segment's writer. The only other store is a newer writer marking it
//   Replaced, after it holds the lock. Readers map it read-only.
template <class Atomics>
struct ControlBlock {
  // Written once by SegmentFormat::construct(); ready_marker last.
  struct alignas(kCacheLineBytes) Identity {
    SharedWord<Atomics> ready_marker;      // kSegmentReadyMarker once the segment is complete
    SharedWord<Atomics> layout_hash;       // BusLayout::layout_hash() of the writer's build
    SharedWord<Atomics> instrument_count;  // records in the snapshot table
  };

  // Written by the writer every 1 ms (heartbeat) and on start, exit or replacement (state).
  struct alignas(kCacheLineBytes) Liveness {
    SharedWord<Atomics> heartbeat_ns;  // steady_clock_ns() of the writer's last heartbeat
    SharedWord<Atomics> writer_state;  // a WriterState; use load/store_writer_state below
  };

  // Written every 64 publishes; a reader starts here on attach and after a lap.
  struct alignas(kCacheLineBytes) HeadHint {
    // The next seq the writer will publish, as of the last hint store; trails the true value by
    // 0..63. After seqs 0..130 are published the hint holds 128.
    SharedWord<Atomics> next_seq;
  };

  Identity identity;
  Liveness liveness;
  HeadHint head_hint;
};
static_assert(sizeof(ControlBlock<StdAtomics>) == 3 * kCacheLineBytes, "3 x 128 B = 384 B");

// Reads writer_state with acquire, the pair of store_writer_state's release.
inline WriterState load_writer_state(const ControlBlock<StdAtomics>& control) {
  return static_cast<WriterState>(StdAtomics::load_acquire(control.liveness.writer_state));
}

inline void store_writer_state(ControlBlock<StdAtomics>& control, WriterState state) {
  StdAtomics::store_release(control.liveness.writer_state, static_cast<std::uint64_t>(state));
}

}  // namespace mdbus
