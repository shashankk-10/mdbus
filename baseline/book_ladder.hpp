#pragma once

// The book ladder: the policies of the book I started from (F0), and the configs that walk from
// it to the final book one policy at a time. The table of steps is at the bottom.
// - Not part of the system: each config is measured on its own (one feed_bench binary per
//   config, named by a CMake macro) and checked against F0 by the oracle test.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "mdbus/book/feed_book.hpp"

namespace mdbus::baseline {

// F0's level policy: one std::map per instrument side, one heap node per price level.
// - Keys are price offsets, negated for bids, so begin() is always the best level and
//   upper_bound() the next worse one on either side.
class StdMapLevels {
 private:
  using Map = std::map<std::int32_t, std::uint32_t>;

  std::vector<Map> maps;  // [2 * instrument_id + side]

 public:
  explicit StdMapLevels(std::uint32_t instrument_count) : maps(2 * std::size_t{instrument_count}) {}

  // Adds qty at a price and returns the level's new total. A new level is a new node from 0.
  std::uint32_t add_qty(std::uint16_t instrument_id, std::uint8_t side, std::int32_t price_offset,
                        std::uint32_t qty) {
    std::uint32_t& total = levels_of(instrument_id, side)[key(side, price_offset)];
    total += qty;
    return total;
  }

  // Takes qty off a level that must exist; erases the node when the total reaches 0.
  std::uint32_t remove_qty(std::uint16_t instrument_id, std::uint8_t side,
                           std::int32_t price_offset, std::uint32_t qty) {
    Map& levels = levels_of(instrument_id, side);
    const auto it = levels.find(key(side, price_offset));
    it->second -= qty;
    const std::uint32_t qty_left = it->second;
    if (qty_left == 0) levels.erase(it);
    return qty_left;
  }

  // The best level strictly worse than price_offset, or book::kNoLevel. Sets qty when found.
  std::int32_t next_worse_level(std::uint16_t instrument_id, std::uint8_t side,
                                std::int32_t price_offset, std::uint32_t& qty) const {
    const Map& levels = levels_of(instrument_id, side);
    const auto it = levels.upper_bound(key(side, price_offset));
    if (it == levels.end()) return book::kNoLevel;
    qty = it->second;
    return key(side, it->first);  // negating twice gives the offset back
  }

 private:
  Map& levels_of(std::uint16_t instrument_id, std::uint8_t side) {
    return maps[2u * instrument_id + side];
  }

  const Map& levels_of(std::uint16_t instrument_id, std::uint8_t side) const {
    return maps[2u * instrument_id + side];
  }

  // Side 0 is bids: negate so the highest bid sorts first.
  static std::int32_t key(std::uint8_t side, std::int32_t price_offset) {
    return side == 0 ? -price_offset : price_offset;
  }
};

// F1's order index: std::unordered_map, one heap node per live order.
// - reserve() sizes the buckets once in the constructor, so an insert under the cap never
//   rehashes. The node allocation per insert is what F1 -> F2 measures.
class UnorderedMapIndex {
 private:
  using Map = std::unordered_map<std::uint64_t, book::OrderRecord>;

  std::uint32_t live_order_cap;
  Map orders_by_id;

 public:
  using Handle = Map::iterator;

  explicit UnorderedMapIndex(unsigned table_log2)
      : live_order_cap(book::max_live_orders_for(table_log2)) {
    orders_by_id.reserve(live_order_cap);
  }

  std::uint32_t live_orders() const {
    return static_cast<std::uint32_t>(orders_by_id.size());
  }

  std::uint32_t max_live_orders() const {
    return live_order_cap;
  }

  Handle find(std::uint64_t id) {
    return orders_by_id.find(id);
  }

  bool is_found(Handle handle) const {
    return handle != orders_by_id.end();
  }

  book::OrderRecord& record(Handle handle) {
    return handle->second;
  }

  bool insert(std::uint64_t id, book::OrderRecord new_record) {
    return orders_by_id.try_emplace(id, new_record).second;
  }

  void erase(Handle handle) {
    orders_by_id.erase(handle);
  }

  void prefetch(std::uint64_t /*id*/) const {}  // a node map has no slot to prefetch

  bool check_invariants() const {
    return true;  // nothing to check: the map keeps itself
  }
};

// F2's lookup policy: find the instrument by its 8-character symbol instead of by its id.
// - Eight characters fit the std::string small-string buffer, so each lookup costs a hash and a
//   compare but no allocation.
class InstrumentLookupBySymbol {
 private:
  std::unordered_map<std::string, std::uint16_t> ids;

 public:
  explicit InstrumentLookupBySymbol(const std::vector<book::InstrumentInfo>& instrument_list) {
    for (std::size_t i = 0; i < instrument_list.size(); ++i) {
      ids.emplace(std::string(instrument_list[i].symbol.data(), book::kSymbolLength),
                  static_cast<std::uint16_t>(i));
    }
  }

  std::uint16_t find_instrument(const book::OrderEvent& event) const {
    const auto it = ids.find(std::string(event.symbol.data(), book::kSymbolLength));
    if (it == ids.end()) return book::kNoInstrument;
    return it->second;
  }
};

// The ladder: five configs, F0..F4, and four steps of one policy each.
//   F0 -> F1  LevelPolicy:          StdMapLevels (a std::map per side) -> PriceLadder
//   F1 -> F2  OrderIndexPolicy:     UnorderedMapIndex (a node per order) -> OrderTable
//   F2 -> F3  LookupPolicy:         InstrumentLookupBySymbol -> InstrumentLookupById
//   F3 -> F4  kPrefetchEventsAhead: 0 -> 4 (F4 is book::FinalBookConfig)
// In code each config is the next one with that step taken back: F3 is F4 without the prefetch,
// and so on down to F0.
struct F4 : book::FinalBookConfig { static constexpr const char* kName = "F4"; };
struct F3 : F4 {
  static constexpr std::size_t kPrefetchEventsAhead = 0;  // no prefetch is issued
  static constexpr const char* kName = "F3";
};
struct F2 : F3 {
  using LookupPolicy = InstrumentLookupBySymbol;
  static constexpr const char* kName = "F2";
};
struct F1 : F2 {
  using OrderIndexPolicy = UnorderedMapIndex;
  static constexpr const char* kName = "F1";
};
struct F0 : F1 { using LevelPolicy = StdMapLevels; static constexpr const char* kName = "F0"; };

// True for the configs that allocate while applying events (F0 and F1): a map node per level
// or per order. bench/feed_bench.cpp exempts them from its page-fault check.
template <class BookConfig>
constexpr bool kAllocatesPerEvent =
    std::is_same_v<typename BookConfig::LevelPolicy, StdMapLevels> ||
    std::is_same_v<typename BookConfig::OrderIndexPolicy, UnorderedMapIndex>;

}  // namespace mdbus::baseline
