// Messages, the layout check and the payload checksum, checked at compile time and on one thread.
// Only what no other test covers: the bus tests already publish and dispatch every message type.
// - A reader refuses a segment written with another layout version or ring geometry.
// - dispatch rejects ids outside the schema.
// - The checksum catches a flipped bit in any word except publish_ticks and the checksum itself.
// - A failure means a reader could attach to a writer with a different layout, or the tests
//   that check the checksum (failure_test) could miss a torn copy.

#include "mdbus/bus_writer.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

using namespace mdbus;

// kContains rejects a type that is not a schema message, so publishing one cannot compile.
static_assert(!DefaultSchema::kContains<MessageHeader>);

// The same layout passes; another slot count, payload size, slot size or layout version is
// LayoutMismatch. Would catch a field the writer stores but the reader does not compare.
TEST(reader_refuses_another_layout) {
  using Written = BusLayout<DefaultSchema, 1024>;
  InProcessBus<Written> bus(1);
  ControlBlock<StdAtomics>& control = *bus.pointers().control;
  const std::size_t bytes = SegmentFormat<Written>::segment_bytes(1);
  CHECK(SegmentFormat<Written>::validate(control, bytes) == Status::Ok);
  CHECK((SegmentFormat<BusLayout<DefaultSchema, 2048>>::validate(control, bytes)) ==
        Status::LayoutMismatch);
  CHECK((SegmentFormat<BusLayout<DefaultSchema, 1024, 15>>::validate(control, bytes)) ==
        Status::LayoutMismatch);
  CHECK((SegmentFormat<BusLayout<DefaultSchema, 1024, 7, 64>>::validate(control, bytes)) ==
        Status::LayoutMismatch);
  StdAtomics::store_relaxed(control.identity.layout_version, kLayoutVersion + 1);
  CHECK(SegmentFormat<Written>::validate(control, bytes) == Status::LayoutMismatch);
}

// Ids outside the schema reach no handler and return false: 0 (reserved, so an all-zero payload
// is no message) and 9. Would catch a dispatch that falls through to some type.
TEST(dispatch_rejects_unknown_ids) {
  int calls = 0;
  auto count_call = [&](const auto&) { ++calls; };
  const std::uint8_t body[Payload<kDefaultPayloadWords>::kMaxBodyBytes] = {};
  CHECK(!DefaultSchema::dispatch(0, body, count_call));
  CHECK(!DefaultSchema::dispatch(9, body, count_call));
  CHECK(calls == 0);
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
