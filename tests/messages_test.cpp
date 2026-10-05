// Messages and slot layout, checked at compile time and on one thread.
// - The stamp plus payload fit the first 64 B line of a slot (static_asserts below).
// - The layout hash changes with everything writer and reader must agree on.
// - dispatch reaches every message type and rejects unknown ids.
// - The checksum catches a flipped bit in any word except publish_ticks and the checksum itself.
// - A failure means a reader could attach to a writer with a different layout, or deliver a
//   corrupted message as good.

#include <type_traits>

#include "mdbus/bus_writer.hpp"
#include "test_harness.hpp"

using namespace mdbus;

// Stamp and payload end exactly at the first 64 B line, so a reader's copy touches one L1 line.
static_assert(offsetof(Slot<StdAtomics>, payload_words) + sizeof(Payload<kDefaultPayloadWords>) ==
              kL1LineBytes);
// The largest message fits the payload body.
static_assert(DefaultSchema::kMaxMessageBytes <= Payload<kDefaultPayloadWords>::kMaxBodyBytes);
// kContains finds a listed message and rejects a type that is not one.
static_assert(DefaultSchema::kContains<Trade>);
static_assert(!DefaultSchema::kContains<MessageHeader>);

// Same layout, same hash; change slot count, payload words, slot bytes, the message list or its
// order, and the hash changes. Would catch a field left out of layout_hash().
TEST(hash_tracks_layout) {
  const std::uint64_t base = BusLayout<>::layout_hash();
  CHECK(base == (BusLayout<DefaultSchema, kDefaultSlotCount>::layout_hash()));
  CHECK(base != (BusLayout<DefaultSchema, 1024>::layout_hash()));  // another slot count
  CHECK(base != (BusLayout<DefaultSchema, kDefaultSlotCount, 15>::layout_hash()));  // 15 words
  // 7 payload words in a 64 B slot instead of 128 B.
  CHECK(base != (BusLayout<DefaultSchema, kDefaultSlotCount, 7, 64>::layout_hash()));
  CHECK(base != (BusLayout<Schema<BookDelta, Trade>>::layout_hash()));  // a message left out
  // The same two messages in the other order.
  const std::uint64_t trade_first = BusLayout<Schema<Trade, BookDelta>>::layout_hash();
  const std::uint64_t delta_first = BusLayout<Schema<BookDelta, Trade>>::layout_hash();
  CHECK(trade_first != delta_first);
}

// Each of the three type ids reaches its handler once; ids 0 and 9 reach none. A Trade's fields
// arrive intact. Would catch a missing or duplicated case in dispatch.
TEST(dispatch_reaches_every_type) {
  constexpr std::uint8_t kLastTypeId = 3;   // BookDelta, Trade, InstrumentStatus are ids 1..3
  constexpr std::uint8_t kUnknownTypeId = 9;
  int seen[kLastTypeId + 1] = {};
  auto count_type = [&](const auto& message) {
    ++seen[std::decay_t<decltype(message)>::kTypeId];
  };
  std::uint8_t body[32] = {};  // the payload body size
  for (std::uint8_t id = 1; id <= kLastTypeId; ++id)
    CHECK(DefaultSchema::dispatch(id, body, count_type));
  CHECK(!DefaultSchema::dispatch(0, body, count_type));
  CHECK(!DefaultSchema::dispatch(kUnknownTypeId, body, count_type));
  for (std::uint8_t id = 1; id <= kLastTypeId; ++id) CHECK(seen[id] == 1);

  Trade trade{-5, 7, 1, {}};
  std::memcpy(body, &trade, sizeof trade);
  bool fields_intact = false;
  DefaultSchema::dispatch(Trade::kTypeId, body, [&](const auto& message) {
    if constexpr (std::is_same_v<std::decay_t<decltype(message)>, Trade>)
      fields_intact = message.price == -5 && message.qty == 7;
  });
  CHECK(fields_intact);
}

// encode_payload fills the header and body, and its checksum covers seq and every word that
// matters. Flips bits 0, 17 and 63 of each word in turn: the checksum must change unless the
// word is publish_ticks or the checksum's own upper half.
TEST(encode_and_checksum) {
  using TestPayload = Payload<kDefaultPayloadWords>;
  constexpr std::uint64_t kSeq = 42;
  constexpr std::uint16_t kInstrumentId = 2;
  constexpr std::uint64_t kLatencyStartTicks = 77;
  const Trade trade{100, 3, 0, {}};
  const auto words = Publisher<>::encode_payload(kSeq, kInstrumentId, trade, kLatencyStartTicks);
  TestPayload payload;
  std::memcpy(&payload, words.data(), sizeof payload);

  CHECK(payload.header.instrument_id == kInstrumentId);
  CHECK(payload.header.type_id == Trade::kTypeId);
  CHECK(payload.header.latency_start_ticks == kLatencyStartTicks);
  CHECK(payload.header.publish_ticks == 0);  // stamped later, at publish
  CHECK(read_message<Trade>(payload.body).price == 100);
  CHECK(payload.header.checksum == payload_checksum(payload, kSeq));
  CHECK(payload_checksum(payload, kSeq) != payload_checksum(payload, kSeq + 1));

  for (std::size_t i = 0; i < kDefaultPayloadWords; ++i) {
    for (int bit : {0, 17, 63}) {  // low, middle and high bit of the word
      auto flipped_words = words;
      flipped_words[i] ^= std::uint64_t{1} << bit;
      TestPayload changed;
      std::memcpy(&changed, flipped_words.data(), sizeof changed);
      const bool checksum_bits = i == kChecksumWord && bit >= 32;
      // publish_ticks and checksum are excluded from the checksum.
      if (i == kPublishTicksWord || checksum_bits)
        CHECK(payload_checksum(changed, kSeq) == payload_checksum(payload, kSeq));
      else
        CHECK(payload_checksum(changed, kSeq) != payload_checksum(payload, kSeq));
    }
  }
}

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
