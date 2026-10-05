#pragma once

// A bus's shared-memory format: its compile-time shape (BusLayout), where each part sits inside
// the segment (sizes, offsets, typed pointers), and how a segment is built and validated.
// - segment: the one shm object of a bus. With the default layout:
//   [ControlBlock 384 B][16384 slots x 128 B = 2 MB][instrument_count x 128 B records],
//   rounded up to 16 KB pages. Only the writer writes it; readers map it read-only.
// - ready marker: Identity::ready_marker, which the writer sets to kSegmentReadyMarker last,
//   with release, once the segment is complete, so a reader that loads it with acquire sees the
//   whole segment.
// - layout check: the writer stores kLayoutVersion and the ring's geometry (slot count, slot
//   bytes, payload words) in the ControlBlock; a reader built with any other value refuses to
//   attach (Status::LayoutMismatch) instead of misreading memory. Everything else that lives in
//   shared memory is covered by bumping kLayoutVersion (constants.hpp).
// - Every bus template takes a Layout and reads its sizes from it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

#include "mdbus/constants.hpp"
#include "mdbus/control_block.hpp"
#include "mdbus/messages.hpp"
#include "mdbus/ring_slot.hpp"
#include "mdbus/snapshot_table.hpp"
#include "mdbus/status.hpp"

namespace mdbus {

// Everything a reader and the writer must agree on at compile time: the messages, the ring's
// slot count, payload words per slot and slot size.
// - The bus uses the defaults. Tests use small slot counts; PayloadWordCount and SlotBytes exist
//   for the Copy15 and Pad64 variants (baseline/bus_variants.hpp).
// - Every shape rule is checked here, the one place a user picks the numbers.
template <class MessageSchema = DefaultSchema, std::size_t SlotCount = kDefaultSlotCount,
          std::size_t PayloadWordCount = kDefaultPayloadWords,
          std::size_t SlotBytes = kCacheLineBytes>
struct BusLayout {
  using Schema = MessageSchema;
  static constexpr std::size_t kSlotCount = SlotCount;
  static constexpr std::size_t kPayloadWords = PayloadWordCount;
  static constexpr std::size_t kSlotBytes = SlotBytes;
  // One slot's payload as the writer and readers copy it.
  using PayloadWords = std::array<std::uint64_t, PayloadWordCount>;

  static_assert((SlotCount & (SlotCount - 1)) == 0 && SlotCount >= 2,
                "slot count must be a power of two (slot index = seq & (count - 1))");
  static_assert(MessageSchema::kMaxMessageBytes <= Payload<PayloadWordCount>::kMaxBodyBytes,
                "the largest message must fit in a payload's body");
  static_assert((PayloadWordCount + 1) * kWordBytes <= SlotBytes,
                "stamp and payload must fit the slot");
};

// `value` rounded up to the next multiple of `multiple` (any positive number, not only powers of
// two).
constexpr std::size_t round_up_to_multiple(std::size_t value, std::size_t multiple) {
  return (value + multiple - 1) / multiple * multiple;
}

// Typed pointers to the three parts of one segment, as pointers_into() or construct() return
// them. They do not know or care how the memory was mapped.
template <class Layout>
struct SegmentPointers {
  ControlBlock<StdAtomics>* control = nullptr;
  Slot<StdAtomics, Layout::kPayloadWords, Layout::kSlotBytes>* slots = nullptr;
  SnapshotRecord<StdAtomics>* snapshot_records = nullptr;
  std::uint32_t instrument_count = 0;  // records in the snapshot table
};

// Where each part of a segment sits, and how a segment is built and checked.
template <class Layout>
struct SegmentFormat {
  using RingSlot = Slot<StdAtomics, Layout::kPayloadWords, Layout::kSlotBytes>;

  static constexpr std::size_t kRingOffset = sizeof(ControlBlock<StdAtomics>);  // 384 B
  // 384 + 16384 x 128 = 2,097,536 with the default layout.
  static constexpr std::size_t kSnapshotTableOffset =
      kRingOffset + Layout::kSlotCount * sizeof(RingSlot);

  // Bytes to map for a bus with `instrument_count` snapshot records, in whole pages.
  // Example (default layout): segment_bytes(1024) = 2,097,536 + 1024 x 128 = 2,228,608, rounded
  //   up to 16 KB pages = 2,244,608. segment_bytes(1) = 2,113,536.
  static constexpr std::size_t segment_bytes(std::uint32_t instrument_count) {
    const std::size_t snapshot_table_bytes =
        std::size_t{instrument_count} * sizeof(SnapshotRecord<StdAtomics>);
    return round_up_to_multiple(kSnapshotTableOffset + snapshot_table_bytes, kPageSize);
  }

  // Typed pointers into a segment whose objects the writer's construct() already built; readers
  // map the segment and call this.
  static SegmentPointers<Layout> pointers_into(void* segment_start,
                                               std::uint32_t instrument_count) {
    char* first_byte = static_cast<char*>(segment_start);
    SegmentPointers<Layout> result;
    result.control = reinterpret_cast<ControlBlock<StdAtomics>*>(first_byte);
    result.slots = reinterpret_cast<RingSlot*>(first_byte + kRingOffset);
    result.snapshot_records =
        reinterpret_cast<SnapshotRecord<StdAtomics>*>(first_byte + kSnapshotTableOffset);
    result.instrument_count = instrument_count;
    return result;
  }

  // Builds a bus in a new zero-filled segment; writer only.
  // - Placement new starts the lifetime of every std::atomic word in the segment. Readers never
  //   construct anything: they map the segment and cast (pointers_into).
  static SegmentPointers<Layout> construct(void* segment_start, std::uint32_t instrument_count) {
    // 1. Construct the control block, every slot and every snapshot record in place.
    char* first_byte = static_cast<char*>(segment_start);
    new (first_byte) ControlBlock<StdAtomics>();
    for (std::size_t i = 0; i < Layout::kSlotCount; ++i)
      new (first_byte + kRingOffset + i * sizeof(RingSlot)) RingSlot();
    for (std::size_t i = 0; i < instrument_count; ++i)
      new (first_byte + kSnapshotTableOffset + i * sizeof(SnapshotRecord<StdAtomics>))
          SnapshotRecord<StdAtomics>();

    // 2. Fill Identity, ready marker last.
    const SegmentPointers<Layout> result = pointers_into(segment_start, instrument_count);
    auto& identity = result.control->identity;
    StdAtomics::store_relaxed(identity.layout_version, kLayoutVersion);
    StdAtomics::store_relaxed(identity.slot_count, Layout::kSlotCount);
    StdAtomics::store_relaxed(identity.slot_bytes, Layout::kSlotBytes);
    StdAtomics::store_relaxed(identity.payload_words, Layout::kPayloadWords);
    StdAtomics::store_relaxed(identity.instrument_count, instrument_count);
    StdAtomics::store_release(identity.ready_marker, kSegmentReadyMarker);
    return result;
  }

  // Checks a mapped segment against what this binary was compiled for, in this order:
  // - ready marker 0 -> NotReady (the writer is still building it); any other wrong value ->
  //   NotABusSegment.
  // - layout version, slot count, slot bytes or payload words differ -> LayoutMismatch.
  // - instrument count 0 or over kMaxInstrumentCount, or a mapping smaller than segment_bytes()
  //   of that count -> SizeMismatch.
  static Status validate(const ControlBlock<StdAtomics>& control, std::size_t mapped_bytes) {
    const std::uint64_t ready_marker = StdAtomics::load_acquire(control.identity.ready_marker);
    if (ready_marker == 0) return Status::NotReady;
    if (ready_marker != kSegmentReadyMarker) return Status::NotABusSegment;
    const auto& identity = control.identity;
    if (StdAtomics::load_relaxed(identity.layout_version) != kLayoutVersion ||
        StdAtomics::load_relaxed(identity.slot_count) != Layout::kSlotCount ||
        StdAtomics::load_relaxed(identity.slot_bytes) != Layout::kSlotBytes ||
        StdAtomics::load_relaxed(identity.payload_words) != Layout::kPayloadWords)
      return Status::LayoutMismatch;
    const std::uint64_t instrument_count =
        StdAtomics::load_relaxed(control.identity.instrument_count);
    if (instrument_count == 0 || instrument_count > kMaxInstrumentCount)
      return Status::SizeMismatch;
    if (mapped_bytes < segment_bytes(static_cast<std::uint32_t>(instrument_count)))
      return Status::SizeMismatch;
    return Status::Ok;
  }
};

}  // namespace mdbus
