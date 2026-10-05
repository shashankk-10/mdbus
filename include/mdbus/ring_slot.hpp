#pragma once

// One ring slot: the stamp, the message header, the payload words, and the payload checksum.
// - Byte map of a default slot:
//   [stamp 8 B][header 24 B][body 32 B] = 64 B used | 64 B padding = 128 B
// - Publisher builds a Payload in registers and RingWriter copies its words into a Slot; Consumer
//   copies them back out and reads the MessageHeader.
// - Slot is a template on an atomic policy (atomic_policy.hpp): the bus uses StdAtomics, the test
//   mutants use weakened copies of it.
// - stamp: the slot's version word; its encoding is defined once, in ring.hpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mdbus/atomic_policy.hpp"
#include "mdbus/constants.hpp"

namespace mdbus {

// The first 24 B of every payload.
// - latency_start_ticks: the tick an end-to-end latency is measured from (the bench's scheduled
//   send tick, or the feed handler's packet-receive tick); 0 when nobody measures.
// - No seq field: the stamp already proves which seq the slot holds.
struct MessageHeader {
  std::uint64_t latency_start_ticks;
  std::uint64_t publish_ticks;  // read right before the publish, see write_publish_ticks
  std::uint16_t instrument_id;
  std::uint8_t type_id;    // the message's kTypeId (messages.hpp)
  std::uint8_t padding;    // always 0: it is inside the checksum
  std::uint32_t checksum;  // payload_checksum() of the payload and its seq
};
static_assert(sizeof(MessageHeader) == 24, "8 + 8 + 2 + 1 + 1 + 4 = 24 B");

// Where each header field sits in a payload's 8 B words.
constexpr std::size_t kLatencyStartTicksWord =
    offsetof(MessageHeader, latency_start_ticks) / kWordBytes;  // word 0
constexpr std::size_t kPublishTicksWord =
    offsetof(MessageHeader, publish_ticks) / kWordBytes;  // word 1
// Word 2, shared with instrument_id (bits 0-15), type_id (16-23) and padding (24-31).
constexpr std::size_t kChecksumWord = offsetof(MessageHeader, checksum) / kWordBytes;

// A slot's payload as the writer builds it in registers: the header, then the message body.
// - Copied as whole 8 B words, so its size is always PayloadWordCount x 8 B.
template <std::size_t PayloadWordCount>
struct Payload {
  static_assert(PayloadWordCount * kWordBytes > sizeof(MessageHeader));
  static constexpr std::size_t kMaxBodyBytes =
      PayloadWordCount * kWordBytes - sizeof(MessageHeader);

  MessageHeader header;
  std::uint8_t body[kMaxBodyBytes];  // the message, then zeros up to kMaxBodyBytes
};
// 7 words = 56 B: 24 B header + 32 B body (kMaxBodyBytes).
static_assert(sizeof(Payload<kDefaultPayloadWords>) == 56 &&
              Payload<kDefaultPayloadWords>::kMaxBodyBytes == 32);

// One cell of the ring.
// - With the default 7 words, stamp + payload fill the first 64 B L1 unit and the second 64 B is
//   padding, so two slots never share a 128 B cache line.
// - Why not 64 B slots: two slots would then share each 128 B line, so a reader on the other
//   cluster copying slot s would contend with the writer storing s + 1.
// - Measured: 64 B slots are a little cheaper per publish at saturation, the same at 1 M msg/s.
//   I keep 128 B as a hedge because macOS will not pin a reader to a cluster (DESIGN.md, Layout).
// - SlotBytes 64 exists for that Pad64 variant only.
template <class Atomics, std::size_t PayloadWordCount = kDefaultPayloadWords,
          std::size_t SlotBytes = kCacheLineBytes>
struct alignas(SlotBytes) Slot {
  SharedWord<Atomics> stamp;
  SharedWord<Atomics> payload_words[PayloadWordCount];
};
static_assert(sizeof(Slot<StdAtomics>) == kCacheLineBytes &&
              alignof(Slot<StdAtomics>) == kCacheLineBytes);
static_assert(offsetof(Slot<StdAtomics>, payload_words) == kWordBytes &&
                  sizeof(Slot<StdAtomics>::payload_words) + kWordBytes == kL1LineBytes,
              "stamp + payload are exactly one 64 B L1 unit");

// Payload checksum.

// 2^64 / golden ratio (1.618...), an odd number. A common choice for multiplicative hashing.
// - Multiplying mixes each input bit into the bits above it, so the top bits of the product are
//   the well-mixed ones. That is why the checksum folds them down (x ^ (x >> 29)) before keeping
//   the low 32 bits.
// - Odd, so the multiply never loses information.
// - book/order_table.hpp uses the same number for its hash (kHashMultiplier).
constexpr std::uint64_t kChecksumMultiplier = 0x9E3779B97F4A7C15;

// Copies high bits down so the final multiply mixes them into the low 32 bits we keep; a common
// mixer shift, not tuned.
constexpr unsigned kChecksumMixShift = 29;

// A 32-bit hash of one payload and the seq it was published at.
// - Only tests check it (failure_test, messages_test), to catch a torn copy. Production readers
//   skip it: the stamp re-check already rejects torn copies.
// - The writer computes it on every publish, a few independent multiplies, before the clock read
//   (see read_ticks_after), so it stays out of the measured hop.
// - publish_ticks is written after the checksum, so it is left out (zeroed in the copy), and so
//   is the checksum field itself.
// - It is 32 bits, so a changed payload goes unnoticed about once in 2^32: enough for tests.
// Example (the cases tests/messages_test checks):
//   payload_checksum(payload, 42) != payload_checksum(payload, 43)  same bytes, different seq
//   flip any bit of the body or of instrument_id                    -> a different value
//   change payload.header.publish_ticks                             -> the same value
template <std::size_t WordCount>
std::uint32_t payload_checksum(const Payload<WordCount>& payload, std::uint64_t seq) {
  constexpr unsigned kHalfWordBits = 32;  // a 64-bit word folds into the 32-bit checksum field

  Payload<WordCount> copy = payload;
  copy.header.publish_ticks = 0;
  copy.header.checksum = 0;
  std::array<std::uint64_t, WordCount> words;
  std::memcpy(words.data(), &copy, sizeof words);

  // Each word gets its own odd multiplier (kChecksumMultiplier + 2, + 4, ...), so swapping two
  // words changes the sum.
  std::uint64_t hash = seq * kChecksumMultiplier;
  for (std::size_t i = 0; i < WordCount; ++i)
    hash += words[i] * (kChecksumMultiplier + 2 * (i + 1));
  hash ^= hash >> kChecksumMixShift;
  hash *= kChecksumMultiplier;
  return static_cast<std::uint32_t>(hash ^ (hash >> kHalfWordBits));
}

}  // namespace mdbus
