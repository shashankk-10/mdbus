#pragma once

// One ring slot: the stamp, the message header, the payload words, and the payload checksum.
// - Byte map of a default slot:
//   [stamp 8 B][header 24 B][body 32 B] = 64 B used | 64 B padding = 128 B
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
// - Measured: 64 B slots are a little cheaper per publish at saturation, with no resolved
//   difference at 1 M msg/s. I keep 128 B as a hedge because macOS will not pin a reader to a
//   cluster (DESIGN.md §7).
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

// Copies high bits (the well-mixed ones of a multiply) down, so the final multiply mixes them into
// the low 32 bits we keep; a common mixer shift, not tuned.
constexpr unsigned kChecksumMixShift = 29;

// A 32-bit hash of one payload and the seq it was published at.
// - Only tests check it, to catch a torn copy. Production readers skip it: the stamp re-check
//   already rejects torn copies.
// - The writer still computes it on every publish: about 26 instructions for a BookDelta, a
//   serial chain of dependent multiply-adds. It runs before the publish tick is read
//   (read_ticks_after), so the hop leaves it out, and the saturated-writer benchmark (W-sat)
//   publishes words encoded beforehand, so that leaves it out too. Only e2e spans it, and even
//   e2e does not show it: the paced writer publishes inside its due tick (e2e minus hop body
//   mean 0.03 ns, results/runs.csv).
// - publish_ticks is written after the checksum, so it is left out (zeroed in the copy), and so
//   is the checksum field itself.
// - It is 32 bits, so a changed payload goes unnoticed about once in 2^32: enough for tests.
template <std::size_t WordCount>
std::uint32_t payload_checksum(const Payload<WordCount>& payload, std::uint64_t seq) {
  Payload<WordCount> copy = payload;
  copy.header.publish_ticks = 0;
  copy.header.checksum = 0;
  std::array<std::uint64_t, WordCount> words;
  std::memcpy(words.data(), &copy, sizeof words);

  // Each word gets its own odd multiplier (kGoldenRatioMultiplier + 2, + 4, ...), so swapping
  // two words changes the sum.
  std::uint64_t hash = seq * kGoldenRatioMultiplier;
  for (std::size_t i = 0; i < WordCount; ++i)
    hash += words[i] * (kGoldenRatioMultiplier + 2 * (i + 1));
  hash ^= hash >> kChecksumMixShift;
  hash *= kGoldenRatioMultiplier;
  return static_cast<std::uint32_t>(hash ^ (hash >> 32));
}

}  // namespace mdbus
