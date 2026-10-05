#pragma once

// The first thing in every bus segment: the ready marker, the writer's state, and the head hint.

#include <cstddef>
#include <cstdint>

#include "mdbus/atomic_policy.hpp"
#include "mdbus/constants.hpp"

namespace mdbus {

// The segment's ready marker (segment_format.hpp says how it is stored and checked). In memory
// (little-endian) its bytes are the ASCII text "MDBUSRDY", so `hexdump -C` of the segment shows
// the word at offset 0.
// - It is not a version number; the Identity fields after it do that job.
constexpr std::uint64_t kSegmentReadyMarker =
    0x5944'5253'5542'444d;  // bytes 4d 44 42 55 53 52 44 59

// What the segment's writer is doing, as readers see it: Running from the start (a fresh
// segment is all zeros), Exited after a clean shutdown (Publisher::mark_exited). A writer that
// dies without saying so stays Running; readers then go by the heartbeat (writer_liveness.hpp).
enum class WriterState : std::uint64_t { Running, Exited };

// The 384 B block at offset 0 of every bus segment.
// - Three parts, each on its own 128 B line, because each is written at a different rate:
//   identity once, liveness at most every 1 ms, the head hint every 64 publishes. A store to one
//   part never takes away the line holding another (false sharing).
// - Written only by this segment's writer. Readers map it read-only.
template <class Atomics>
struct ControlBlock {
  // Written once by SegmentFormat::construct(); ready_marker last. A reader compares every
  // field with its own build before it attaches (SegmentFormat::validate).
  struct alignas(kCacheLineBytes) Identity {
    SharedWord<Atomics> ready_marker;      // kSegmentReadyMarker once the segment is complete
    SharedWord<Atomics> layout_version;    // kLayoutVersion of the writer's build
    SharedWord<Atomics> slot_count;        // the ring's geometry in the writer's build
    SharedWord<Atomics> slot_bytes;
    SharedWord<Atomics> payload_words;
    SharedWord<Atomics> instrument_count;  // records in the snapshot table
  };

  struct alignas(kCacheLineBytes) Liveness {
    SharedWord<Atomics> heartbeat_ns;  // steady_clock_ns() of the writer's last heartbeat
    SharedWord<Atomics> writer_state;  // a WriterState; use load/store_writer_state below
  };

  struct alignas(kCacheLineBytes) HeadHint {
    SharedWord<Atomics> next_seq;  // the head hint (ring.hpp)
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
