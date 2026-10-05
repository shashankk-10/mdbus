#pragma once

// A bus's compile-time shape (BusLayout), and the hash that lets a reader refuse a writer built
// with a different one.
// - Every template in the bus (RingWriter, Publisher, Consumer, SegmentFormat...) takes a Layout
//   and reads its sizes from here.
// - The writer stores layout_hash() in the ControlBlock; SegmentFormat::validate() compares it
//   on attach and returns Status::LayoutMismatch when it differs.

#include <array>
#include <cstddef>
#include <cstdint>

#include "mdbus/constants.hpp"
#include "mdbus/control_block.hpp"
#include "mdbus/messages.hpp"
#include "mdbus/ring_slot.hpp"
#include "mdbus/snapshot_table.hpp"

namespace mdbus {

// Hashes a list of 64-bit numbers (the layout's sizes and ids) into one 64-bit value.
// - The algorithm is FNV-1a, a small, well-known hash: for each byte,
//   hash = (hash XOR byte) * kFnvPrime.
// - Why a hash at all: the writer stores the hash of its BusLayout in the segment; a reader
//   compiled with a different one (slot count, payload words, message list...) sees a different
//   hash and refuses to attach instead of misreading memory.
// - Why FNV-1a: it is constexpr-friendly and ten lines long; speed does not matter (it runs at
//   compile time).
struct LayoutHasher {
  // The two standard 64-bit FNV-1a values, copied from the published algorithm, not chosen here.
  // - kFnvStartValue is what the FNV spec calls the "offset basis": the hash of no input.
  // - kFnvPrime is 2^40 + 0x1b3, a prime the FNV authors picked so each multiply spreads one
  //   byte across all 64 bits.
  static constexpr std::uint64_t kFnvStartValue = 0xcbf29ce484222325;
  static constexpr std::uint64_t kFnvPrime = 0x100000001b3;
  static constexpr unsigned kBitsPerByte = 8;
  static constexpr std::uint64_t kLowByteMask = 0xff;  // keeps one byte after the shift

  std::uint64_t hash = kFnvStartValue;

  // Feeds the 8 bytes of `input` into the hash, lowest byte first.
  // Example: LayoutHasher hasher; hasher.add(5); hasher.add(128); -> hasher.hash is a fixed
  //   64-bit value; adding the same numbers in another order gives a different one.
  constexpr void add(std::uint64_t input) {
    for (std::size_t i = 0; i < kWordBytes; ++i) {
      const std::uint64_t byte = (input >> (kBitsPerByte * i)) & kLowByteMask;
      hash = (hash ^ byte) * kFnvPrime;
    }
  }
};

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
  static_assert(sizeof(Payload<PayloadWordCount>) == PayloadWordCount * kWordBytes,
                "a payload is copied as whole 8-byte words");

  // The hash of every size and id both sides must agree on, in a fixed order.
  // - A reorder of two same-size fields is not caught unless kLayoutVersion is bumped.
  static constexpr std::uint64_t layout_hash() {
    LayoutHasher hasher;
    hasher.add(kLayoutVersion);
    hasher.add(kCacheLineBytes);
    hasher.add(SlotCount);
    hasher.add(PayloadWordCount);
    hasher.add(SlotBytes);
    hasher.add(sizeof(ControlBlock<StdAtomics>));
    hasher.add(sizeof(MessageHeader));
    hasher.add(sizeof(InstrumentSnapshot));
    // Id then size per message, so swapping two messages in the schema changes the hash.
    for (std::size_t i = 0; i < MessageSchema::kMessageCount; ++i) {
      hasher.add(MessageSchema::kTypeIds[i]);
      hasher.add(MessageSchema::kMessageBytes[i]);
    }
    return hasher.hash;
  }
};

}  // namespace mdbus
