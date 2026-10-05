#pragma once

// Where each part of a bus sits inside its shared-memory segment: sizes, offsets, typed pointers,
// build and validate.
// - Segment map with the default layout:
//   [ControlBlock 384 B][16384 slots x 128 B = 2 MB][instrument_count x 128 B records],
//   rounded up to 16 KB pages.
// - BusWriter calls segment_bytes() and construct() on a new segment; BusReader calls validate()
//   and pointers_into() on a mapped one.
// - Only the writer writes the segment; readers map it read-only.

#include <cstddef>
#include <cstdint>
#include <new>

#include "mdbus/bus_layout.hpp"
#include "mdbus/constants.hpp"
#include "mdbus/status.hpp"

namespace mdbus {

// `value` rounded up to the next multiple of `multiple` (any positive number, not only powers of
// two).
// Example: round_up_to_multiple(2'228'608, 16384) == 2'244'608 (137 pages),
//   round_up_to_multiple(16384, 16384) == 16384.
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

// The segment of one bus: ControlBlock, the ring, then one snapshot record per instrument, in
// whole pages.
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
  // Example (default layout, segment mapped at address A):
  //   control = A, slots = A + 384, snapshot_records = A + 2,097,536
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
  // - The ready marker is stored last, with release: a reader that sees it sees the whole
  //   Identity and every constructed object.
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
    StdAtomics::store_relaxed(identity.layout_hash, Layout::layout_hash());
    StdAtomics::store_relaxed(identity.instrument_count, instrument_count);
    StdAtomics::store_release(identity.ready_marker, kSegmentReadyMarker);
    return result;
  }

  // Checks a mapped segment against what this binary was compiled for, in this order:
  // - ready marker 0 -> NotReady (the writer is still building it); any other wrong value ->
  //   NotABusSegment.
  // - layout hash differs -> LayoutMismatch.
  // - instrument count 0 or over kMaxInstrumentCount, or a mapping smaller than segment_bytes()
  //   of that count -> SizeMismatch.
  static Status validate(const ControlBlock<StdAtomics>& control, std::size_t mapped_bytes) {
    const std::uint64_t ready_marker = StdAtomics::load_acquire(control.identity.ready_marker);
    if (ready_marker == 0) return Status::NotReady;
    if (ready_marker != kSegmentReadyMarker) return Status::NotABusSegment;
    if (StdAtomics::load_relaxed(control.identity.layout_hash) != Layout::layout_hash())
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
