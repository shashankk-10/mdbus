#pragma once

// The bus benchmarks' workload.
// - Input side: a pool of synthetic book events, made from a seed and encoded into payload words
//   once, before any timing starts. The writer replays the pool in a loop.
// - Reader side: SimulatedReaderWork, the work the fast reader does per message, and the
//   dispatch that feeds it (also what dispatch_bench measures).
// - Used by bus_bench and dispatch_bench.

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "mdbus/bus_writer.hpp"
#include "mdbus/consumer.hpp"

namespace mdbus::bench {

// Input side.

// One synthetic event: one top-of-book level on one side of one instrument.
struct SyntheticDelta {
  std::uint16_t instrument_id;
  BookDelta delta;
};

constexpr std::size_t kEventPoolSize = 1024;  // a power of two: an index % kEventPoolSize is a mask
constexpr std::size_t kPayloadsPerTrade = 10;  // one pool payload in ten is a trade
constexpr std::int32_t kSyntheticBasePrice = 1000;      // lowest price in the pool
constexpr std::uint32_t kSyntheticPriceLevels = 64;     // prices fall in [1000, 1064)
constexpr std::uint32_t kSyntheticMaxQty = 65536;       // quantities fall in [0, 65536)
constexpr std::uint32_t kSideCount = 2;                 // bid or ask

// kEventPoolSize events from seed. The same seed gives the same pool in every process.
inline std::vector<SyntheticDelta> make_event_pool(std::uint64_t seed) {
  std::mt19937 random_engine(static_cast<std::mt19937::result_type>(seed));
  std::vector<SyntheticDelta> events(kEventPoolSize);
  for (SyntheticDelta& event : events) {
    event.instrument_id = static_cast<std::uint16_t>(random_engine() % kDefaultInstrumentCount);
    event.delta = BookDelta{};
    event.delta.side = static_cast<std::uint8_t>(random_engine() % kSideCount);
    event.delta.entry_count = 1;
    event.delta.entries[0].price =
        kSyntheticBasePrice + static_cast<std::int32_t>(random_engine() % kSyntheticPriceLevels);
    event.delta.entries[0].qty = static_cast<std::uint32_t>(random_engine() % kSyntheticMaxQty);
  }
  return events;
}

// The pool as payload words of Layout, at seqs 0..kEventPoolSize-1: what W-sat (unpaced, the
// writer publishes flat out) and the dispatch benchmark replay.
template <class Layout>
using EncodedPayloadPool = std::array<typename Layout::PayloadWords, kEventPoolSize>;

// Encodes every event; one in ten becomes a trade at the event's price and size instead.
template <class Layout>
void encode_event_pool(const std::vector<SyntheticDelta>& events,
                       EncodedPayloadPool<Layout>& out) {
  for (std::size_t event_index = 0; event_index < kEventPoolSize; ++event_index) {
    const SyntheticDelta& event = events[event_index];
    if (event_index % kPayloadsPerTrade == kPayloadsPerTrade - 1) {
      Trade trade{};
      trade.price = event.delta.entries[0].price;
      trade.qty = event.delta.entries[0].qty;
      trade.aggressor = kSellerAggressor;
      out[event_index] = Publisher<Layout>::encode_payload(event_index, event.instrument_id,
                                                           trade, 0);
    } else {
      out[event_index] = Publisher<Layout>::encode_payload(event_index, event.instrument_id,
                                                           event.delta, 0);
    }
  }
}

// Reader side.

// The fast reader's work per message, and what the dispatch benchmark dispatches into.
// - Every handler adds into a member, so the result is used and the compiler cannot delete the
//   dispatch that leads to it.
struct SimulatedReaderWork {
  std::array<std::int64_t, kDefaultInstrumentCount> price{};
  std::uint64_t traded = 0;
  std::uint64_t status_messages = 0;

  void on(const BookDelta& delta, std::uint16_t instrument_id) {
    price[instrument_id % price.size()] += delta.entries[0].price;
  }

  void on(const Trade& trade, std::uint16_t) {
    traded += trade.qty;
  }

  void on(const InstrumentStatus&, std::uint16_t) {
    ++status_messages;
  }
};

// Static dispatch of one payload into SimulatedReaderWork: what Consumer<Derived> compiles to.
template <std::size_t WordCount>
void dispatch_to_work(SimulatedReaderWork& work,
                      const std::array<std::uint64_t, WordCount>& words) {
  Payload<WordCount> payload;
  std::memcpy(&payload, words.data(), sizeof payload);
  DefaultSchema::dispatch(payload.header.type_id, payload.body, [&](const auto& message) {
    work.on(message, payload.header.instrument_id);
  });
}

}  // namespace mdbus::bench
