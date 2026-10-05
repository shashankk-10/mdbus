// The ring protocol on one thread, with no races: only the stamp rules are under test.
// - try_poll returns NotWrittenYet, Ok and Lapped exactly at their boundaries.
// - An odd stamp (writer mid-slot) reads as NotWrittenYet, never as data.
// - After several wraps only the newest lap is readable, and the head hint lands where its
//   publish cadence says.
// - A failure means a reader could take a slot it should not, or skip one it should read.

#include "mdbus/bus_layout.hpp"
#include "mdbus/ring.hpp"
#include "test_harness.hpp"
#include "test_helpers.hpp"

using namespace mdbus;

namespace {
constexpr std::uint64_t kRingSlots = 128;  // small, so a few hundred publishes wrap it
using TestLayout = BusLayout<DefaultSchema, kRingSlots>;
using PayloadWords = TestLayout::PayloadWords;
constexpr std::uint32_t kInstrumentCount = 4;

// The test payload for seq: the shared pattern, sized for this layout.
PayloadWords pattern_words(std::uint64_t seq) {
  return mdbus::pattern_words<kDefaultPayloadWords>(seq);
}

// A writer and a reader on one in-process ring.
struct RingFixture {
  InProcessBus<TestLayout> bus{kInstrumentCount};
  RingWriter<StdAtomics, TestLayout> writer{bus.pointers().slots, bus.pointers().control};
  RingReader<StdAtomics, TestLayout> reader{bus.pointers().slots, bus.pointers().control};

  // Publishes the next `count` seqs, each with its pattern payload.
  void publish_n(std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) writer.publish(pattern_words(writer.next_seq()));
  }
};
}  // namespace

// Each result at its boundary: before the first publish, right after it, and one lap later.
// Would catch an off-by-one in the stamp comparison or the slot index.
TEST(not_yet_ok_lapped) {
  RingFixture fixture;
  PayloadWords out;
  CHECK(fixture.reader.try_poll(0, out) == TryPollResult::NotWrittenYet);
  fixture.publish_n(1);
  REQUIRE(fixture.reader.try_poll(0, out) == TryPollResult::Ok);
  CHECK(out == pattern_words(0));
  CHECK(fixture.reader.try_poll(1, out) == TryPollResult::NotWrittenYet);

  fixture.publish_n(kRingSlots);  // seq 128 overwrites seq 0's slot
  CHECK(fixture.reader.try_poll(0, out) == TryPollResult::Lapped);
  CHECK(fixture.reader.try_poll(kRingSlots, out) == TryPollResult::Ok);
  CHECK(out == pattern_words(kRingSlots));
  CHECK(fixture.reader.try_poll(kRingSlots + 1, out) == TryPollResult::NotWrittenYet);
}

// A slot whose stamp is odd is being written: the reader must wait, and its neighbour stays
// readable. Would catch a reader that only compares stamps for "greater or equal".
TEST(odd_stamp_is_not_yet) {
  constexpr std::uint64_t kSeqBeingWritten = 5;
  RingFixture fixture;
  fixture.publish_n(kSeqBeingWritten);
  StdAtomics::store_relaxed(fixture.bus.pointers().slots[kSeqBeingWritten].stamp,
                            stamp_while_writing(kSeqBeingWritten));
  PayloadWords out;
  CHECK(fixture.reader.try_poll(kSeqBeingWritten, out) == TryPollResult::NotWrittenYet);
  CHECK(fixture.reader.try_poll(kSeqBeingWritten - 1, out) == TryPollResult::Ok);
  CHECK(out == pattern_words(kSeqBeingWritten - 1));
}

// After three and a bit laps, every seq older than the last lap is Lapped and the rest are Ok.
// The head hint is stored every 64 publishes, after the done stamp, so it sits at 384, not 389.
TEST(wrap_and_hint) {
  constexpr std::uint64_t kPublished = 3 * kRingSlots + 5;
  constexpr std::uint64_t kOldestKept = kPublished - kRingSlots;  // 261
  RingFixture fixture;
  fixture.publish_n(kPublished);
  PayloadWords out;
  for (std::uint64_t seq = 0; seq < kPublished; ++seq) {
    const TryPollResult poll_result = fixture.reader.try_poll(seq, out);
    CHECK(poll_result == (seq < kOldestKept ? TryPollResult::Lapped : TryPollResult::Ok));
    if (poll_result == TryPollResult::Ok) CHECK(out == pattern_words(seq));
  }
  CHECK(fixture.reader.head_hint() == 3 * kRingSlots);
}

int main(int argc, char** argv) {
  return mdbus_test::run_main(argc, argv);
}
