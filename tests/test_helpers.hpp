#pragma once

// Test data and fixtures shared by several test files.
// - InProcessBus: a real bus segment in anonymous shared memory, no file names and no lock.
// - Pattern payloads: every word is derived from one version, so a copy that mixes two writes, or
//   is older than expected, fails is_untorn_pattern() or the version check.
// - make_test_snapshot(), top_levels_from_snapshot(), apply_delta(): the one test snapshot shape,
//   and a consumer's two book sides rebuilt the way a real consumer rebuilds them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mdbus/book/top_levels.hpp"
#include "mdbus/bus_layout.hpp"
#include "mdbus/constants.hpp"
#include "mdbus/segment_format.hpp"
#include "mdbus/shared_memory.hpp"
#include "mdbus/snapshot_table.hpp"

namespace mdbus {

// The production segment layout in anonymous shared memory, for one process or for children
// made with fork. No names, no flock.
template <class Layout = BusLayout<>>
class InProcessBus {
 private:
  SharedMemoryMapping segment;
  SegmentPointers<Layout> segment_pointers;

 public:
  explicit InProcessBus(std::uint32_t instrument_count = kDefaultInstrumentCount)
      : segment(SharedMemoryMapping::create_anonymous(
            SegmentFormat<Layout>::segment_bytes(instrument_count))),
        segment_pointers(SegmentFormat<Layout>::construct(segment.address(), instrument_count)) {}

  const SegmentPointers<Layout>& pointers() const {
    return segment_pointers;
  }
};

// Word i of a pattern with i > 0 is version * kPatternVersionStride + i, and word 0 is the
// version itself (the seq of a slot, the last_included_seq of a snapshot). A payload has far
// fewer than 1000 words, so no two versions share any word.
constexpr std::uint64_t kPatternVersionStride = 1000;

// The pattern payload for `version`.
template <std::size_t N>
std::array<std::uint64_t, N> pattern_words(std::uint64_t version) {
  std::array<std::uint64_t, N> words;
  words[0] = version;
  for (std::size_t i = 1; i < N; ++i) words[i] = version * kPatternVersionStride + i;
  return words;
}

// True if every word belongs to the version in word 0: the copy came from one write.
template <std::size_t N>
bool is_untorn_pattern(const std::array<std::uint64_t, N>& words) {
  return words == pattern_words<N>(words[0]);
}

// pattern_words() laid over a whole snapshot record.
inline InstrumentSnapshot pattern_snapshot(std::uint64_t version) {
  const std::array<std::uint64_t, kSnapshotWords> words = pattern_words<kSnapshotWords>(version);
  InstrumentSnapshot snapshot;
  std::memcpy(&snapshot, words.data(), sizeof snapshot);
  return snapshot;
}

inline bool is_untorn_snapshot(const InstrumentSnapshot& snapshot) {
  std::array<std::uint64_t, kSnapshotWords> words;
  std::memcpy(words.data(), &snapshot, sizeof words);
  return is_untorn_pattern(words);
}

// The snapshot a test writer publishes with seq for instrument_id: one bid at price seq, qty
// instrument_id + 1.
inline InstrumentSnapshot make_test_snapshot(std::uint16_t instrument_id, std::uint64_t seq) {
  InstrumentSnapshot snapshot{};
  snapshot.last_included_seq = seq;
  snapshot.instrument_id = instrument_id;
  snapshot.bid_count = 1;
  snapshot.bids[0].price = static_cast<std::int32_t>(seq);
  snapshot.bids[0].qty = instrument_id + 1u;
  return snapshot;
}

// A consumer's copy of one instrument's book: index 0 is the bid side, 1 the ask side (the same
// numbering as BookDelta::side).
using BidAskLevels = std::array<book::TopLevels, 2>;

// The two sides as a recovery snapshot gives them.
inline BidAskLevels top_levels_from_snapshot(const InstrumentSnapshot& snapshot) {
  BidAskLevels sides{};
  sides[0].count = snapshot.bid_count;
  sides[1].count = snapshot.ask_count;
  for (std::size_t k = 0; k < kTopLevelsPerSide; ++k) {
    sides[0].levels[k] = snapshot.bids[k];
    sides[1].levels[k] = snapshot.asks[k];
  }
  return sides;
}

// Applies a BookDelta to a consumer's two sides, by the same rule the writer's book uses.
inline void apply_delta(BidAskLevels& sides, const BookDelta& delta) {
  for (std::size_t k = 0; k < delta.entry_count; ++k) {
    book::apply_level_change(sides[delta.side], delta.side == 0, delta.entries[k]);
  }
}

}  // namespace mdbus
