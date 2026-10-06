#pragma once

// The core protocol: how one message gets into a slot, and how a reader gets it back out.
// - RingWriter::publish() never waits for a reader and does not know how many readers there are.
// - RingReader::try_poll() copies one slot and reports Ok, NotWrittenYet or Lapped.
// - The slot (stamp + 7 payload words, padded to 128 B) is in ring_slot.hpp; the head hint is in
//   control_block.hpp.
// - Stamp of message seq: 2*seq+1 while it is being written, 2*seq+2 once complete, 0 if the slot
//   was never written (below every complete stamp, so try_poll reports NotWrittenYet). Putting seq
//   in the stamp is what lets a reader tell "my message" from "the message one lap later" (a plain
//   seqlock counter cannot).
// - Terms. lap: the writer went once round the ring and overwrote a slot this reader had not
//   read. acquire / release: a release store makes every earlier write visible to whoever later
//   reads that value with an acquire load. head hint: the next seq to publish, stored every 64
//   publishes, so it trails by up to 63.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "mdbus/clock.hpp"
#include "mdbus/control_block.hpp"
#include "mdbus/ring_slot.hpp"

namespace mdbus {

// The stamp the writer stores while it writes message seq, and once the slot holds it.
// Example:
//   stamp_while_writing(41) == 83, stamp_when_written(41) == 84
//   stamp_when_written(16425) == 32852 (seq 41 one lap later in a 16384-slot ring)
constexpr std::uint64_t stamp_while_writing(std::uint64_t seq) {
  return 2 * seq + 1;
}

constexpr std::uint64_t stamp_when_written(std::uint64_t seq) {
  return 2 * seq + 2;
}

// The ring's one writer; the writer's flock keeps it to one process.
// - publish() is wait-free: a bounded number of steps, with no loop, no compare-and-swap and no
//   read of anything a reader writes. Not quite a straight line: every 64th publish also stores
//   the head hint, and every 1024th reads the clock for the heartbeat (about 17 ns, no system
//   call).
// - Layout supplies kSlotCount, kPayloadWords, kSlotBytes and PayloadWords (BusLayout does).
template <class Atomics, class Layout>
class RingWriter {
 public:
  using RingSlot = Slot<Atomics, Layout::kPayloadWords, Layout::kSlotBytes>;
  using PayloadWords = typename Layout::PayloadWords;
  static constexpr std::uint64_t kSlotIndexMask = Layout::kSlotCount - 1;
  // 64, but at most half the ring, so a reader resuming at the hint in a tiny test ring is never a
  // whole lap behind (a 16-slot ring stores it every 8 publishes).
  static constexpr std::uint64_t kPublishesPerHeadHint =
      std::min<std::uint64_t>(kMaxPublishesPerHeadHint, Layout::kSlotCount / 2);

 private:
  RingSlot* slots = nullptr;
  ControlBlock<Atomics>* control = nullptr;
  std::uint64_t next_seq_to_publish = 0;
  std::uint64_t last_heartbeat_ns = 0;

 public:
  RingWriter() = default;
  RingWriter(RingSlot* first_slot, ControlBlock<Atomics>* control_block)
      : slots(first_slot), control(control_block) {}

  // Writes `words` into the slot of the next seq and makes it visible to readers.
  // - Publisher builds the payload in registers and this inlines into it, so between the odd and
  //   the even stamp the writer only stores.
  void publish(const PayloadWords& words) {
    const std::uint64_t seq = next_seq_to_publish;
    next_seq_to_publish = seq + 1;
    RingSlot& slot = slots[seq & kSlotIndexMask];

    // 1. Odd stamp, then a release fence: the fence orders the odd stamp before every payload
    //    store, so a reader whose copy sees any new word also sees a stamp other than the one it
    //    checked first.
    Atomics::store_relaxed(slot.stamp, stamp_while_writing(seq));
    Atomics::fence_release();
    // 2. The payload words (relaxed stores).
    for (std::size_t i = 0; i < Layout::kPayloadWords; ++i)
      Atomics::store_relaxed(slot.payload_words[i], words[i]);
    // 3. Even stamp with release: a reader that loads it with acquire sees every word.
    Atomics::store_release(slot.stamp, stamp_when_written(seq));

    // 4. Bookkeeping, after the even store, so a stall here never leaves a slot odd.
    if ((seq + 1) % kPublishesPerHeadHint == 0)
      Atomics::store_release(control->head_hint.next_seq, seq + 1);
    if ((seq + 1) % kPublishesPerHeartbeatCheck == 0) heartbeat_if_due();
  }

  // Stores a heartbeat if kHeartbeatPeriodNs (1 ms) has passed since the last one.
  void heartbeat_if_due() {
    const std::uint64_t now_ns = steady_clock_ns();
    if (now_ns - last_heartbeat_ns >= kHeartbeatPeriodNs) write_heartbeat(now_ns);
  }

  void write_heartbeat(std::uint64_t now_ns) {
    last_heartbeat_ns = now_ns;
    Atomics::store_release(control->liveness.heartbeat_ns, now_ns);
  }

  std::uint64_t next_seq() const {
    return next_seq_to_publish;
  }
};

// What one try_poll found (try_poll says when each one comes back).
enum class TryPollResult : std::uint8_t { NotWrittenYet, Ok, Lapped };

// One reader's view of the ring. Holds no position: Consumer passes the seq it wants.
// - try_poll() is wait-free: three outcomes, no retry, and the reader stores nothing shared.
template <class Atomics, class Layout>
class RingReader {
 public:
  using RingSlot = Slot<Atomics, Layout::kPayloadWords, Layout::kSlotBytes>;
  using PayloadWords = typename Layout::PayloadWords;
  static constexpr std::uint64_t kSlotIndexMask = Layout::kSlotCount - 1;

 private:
  const RingSlot* slots = nullptr;
  const ControlBlock<Atomics>* control = nullptr;

 public:
  RingReader() = default;
  RingReader(const RingSlot* first_slot, const ControlBlock<Atomics>* control_block)
      : slots(first_slot), control(control_block) {}

  // Copies message `seq` out of its slot, if the slot holds exactly that message.
  // - Ok: `payload` holds message seq. Any other result leaves `payload` meaningless.
  // - NotWrittenYet: the writer has not reached seq, or is in the middle of writing it.
  // - Lapped: the slot already holds a later message, or changed while we copied (a torn copy).
  //   Never retried: the slot only changes again for seq + slot count, so seq is gone.
  // - Why poll the slot and not a global head: one line moves per message instead of two in
  //   series. Measured: polling a head costs 33% more per hop (DESIGN.md §7, "Poll the
  //   slot stamp, not a global counter": 91.3 -> 121.0 ns).
  // Example (16384 slots, so seq 41 lives in slot 41):
  //   stamp 84 before and after the copy  -> Ok
  //   stamp 83 (writing seq 41) or 0      -> NotWrittenYet
  //   stamp 32852 (seq 16425 done)        -> Lapped
  //   stamp 84 before, 32851 after        -> Lapped (torn copy)
  TryPollResult try_poll(std::uint64_t seq, PayloadWords& payload) const {
    const RingSlot& slot = slots[seq & kSlotIndexMask];
    const std::uint64_t expected_stamp = stamp_when_written(seq);

    // 1. Read the stamp. Why acquire and not seq_cst: they differ only in waiting for this
    //    thread's earlier release stores, and a reader makes none (ldapr vs ldar on arm64).
    //    Below expected: odd (being written), 0, or an older lap still in the slot.
    const std::uint64_t stamp_before_copy = Atomics::load_acquire(slot.stamp);
    if (stamp_before_copy < expected_stamp) return TryPollResult::NotWrittenYet;
    if (stamp_before_copy > expected_stamp) return TryPollResult::Lapped;
    // 2. Copy the words (relaxed loads).
    for (std::size_t i = 0; i < Layout::kPayloadWords; ++i)
      payload[i] = Atomics::load_relaxed(slot.payload_words[i]);
    // 3. Acquire fence, then re-read the stamp. If the writer started a later message during the
    //    copy, its release fence makes the new odd stamp visible here.
    Atomics::fence_acquire();
    return Atomics::load_relaxed(slot.stamp) == stamp_before_copy ? TryPollResult::Ok
                                                                  : TryPollResult::Lapped;
  }

  // Read on attach and after a lap.
  std::uint64_t head_hint() const {
    return Atomics::load_acquire(control->head_hint.next_seq);
  }
};

}  // namespace mdbus
