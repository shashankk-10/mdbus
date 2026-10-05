#pragma once

// Broken copies of the publication protocol, run by mutant_stress.cpp on real cores.
// - Each ordering mutant is an atomic policy that weakens one of its operations. Its static
//   member hides the base policy's, so the production ring and snapshot code runs unmodified on
//   top of it, and every call site of that operation is weakened at once.
// - SkipRecheckReader is the one structural mutant: a reader with a missing step. It lives here
//   so that no broken reader ever sits in include/.
// - mutant_stress fails unless each of these shows a bad copy in some cell (stress layout x
//   reader placement). One that survives means the stress test cannot see that kind of bug.

#include <cstddef>
#include <cstdint>

#include "mdbus/control_block.hpp"
#include "mdbus/ring.hpp"
#include "mdbus/snapshot_table.hpp"

namespace mdbus::mutants {

// Writer side: drops the release fence after the odd stamp.
// - The payload stores may then become visible before the "writing" stamp, so a reader can copy
//   half-new words while the stamp still says the old message is complete.
// - The snapshot writer loses the same fence after its odd version.
template <class BaseAtomics>
struct NoReleaseFence : BaseAtomics {
  static void fence_release() {
  }
};

// Reader side: drops the acquire fence between the copy and the stamp re-check.
// - The payload loads may then be satisfied after the re-check, so a slot rewritten mid-copy
//   still passes. The snapshot reader loses the same fence.
template <class BaseAtomics>
struct NoAcquireFence : BaseAtomics {
  static void fence_acquire() {
  }
};

// Writer side: every release store becomes relaxed.
// - The done (even) stamp then no longer publishes the payload stored before it.
// - The head hint, the heartbeat and the snapshot's even version are weakened too.
template <class BaseAtomics>
struct ReleaseStoresRelaxed : BaseAtomics {
  static void store_release(typename BaseAtomics::Word& word, std::uint64_t value) {
    BaseAtomics::store_relaxed(word, value);
  }
};

// Reader side: every acquire load becomes relaxed.
// - Payload loads may then be satisfied before the first stamp load, so a stale copy passes
//   under a fresh stamp.
// - The head hint load and the snapshot version loads are weakened too.
template <class BaseAtomics>
struct AcquireLoadsRelaxed : BaseAtomics {
  static std::uint64_t load_acquire(const typename BaseAtomics::Word& word) {
    return BaseAtomics::load_relaxed(word);
  }
};

// A reader that trusts its copy: no acquire fence and no second stamp read.
// - Its try_poll is RingReader::try_poll copied up to the end of the copy, on purpose. Keep it in
//   step with ring.hpp, or this mutant stops testing the real reader minus one step.
// - A writer that starts the next lap during the copy goes unnoticed, so the stress test must
//   see torn ring copies.
template <class Atomics, class Layout>
struct SkipRecheckReader : RingReader<Atomics, Layout> {
  using Base = RingReader<Atomics, Layout>;
  const typename Base::RingSlot* slots;

  SkipRecheckReader(const typename Base::RingSlot* ring_slots, const ControlBlock<Atomics>* control)
      : Base(ring_slots, control), slots(ring_slots) {}

  TryPollResult try_poll(std::uint64_t seq, typename Base::PayloadWords& payload) const {
    const auto& slot = slots[seq & Base::kSlotIndexMask];
    const std::uint64_t stamp = Atomics::load_acquire(slot.stamp);
    const std::uint64_t expected_stamp = stamp_when_written(seq);
    if (stamp < expected_stamp) return TryPollResult::NotWrittenYet;
    if (stamp > expected_stamp) return TryPollResult::Lapped;
    for (std::size_t i = 0; i < Layout::kPayloadWords; ++i)
      payload[i] = Atomics::load_relaxed(slot.payload_words[i]);
    return TryPollResult::Ok;
  }
};

}  // namespace mdbus::mutants
