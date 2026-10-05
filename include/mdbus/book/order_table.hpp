#pragma once

// FeedBook's order-index policy: order id -> {remaining qty, instrument, side, price offset}.
// - One fixed-size array of 24 B entries, open addressing with linear probing from a
//   multiplicative hash. Entries sit inline: nothing keeps a pointer to an order, because the
//   book publishes totals per price, with no queue per level.
// - A delete shifts later entries back instead of leaving a "deleted" marker, so a long run of
//   adds and cancels never slows the table down and never needs a rehash.
// - Load is capped at 3/4; an insert past the cap is refused (FeedBook marks the instrument
//   kInstrumentBad). The table never grows, so the event path never allocates.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mdbus/constants.hpp"
#include "mdbus/status.hpp"

namespace mdbus::book {

// An order's remaining quantity and where it rests. The price is an offset from the
// instrument's reference_price, which never moves.
struct OrderRecord {
  std::uint32_t remaining_qty;
  std::uint16_t instrument_id;
  std::uint8_t side;  // a Side value: 0 bid, 1 ask
  std::int32_t price_offset;
};

// So order id 0 can never be stored: 0 marks an empty entry.
constexpr std::uint64_t kEmptyEntryId = 0;

struct OrderTableEntry {
  std::uint64_t id;  // kEmptyEntryId when the entry is free
  OrderRecord record;
};
static_assert(sizeof(OrderTableEntry) == 24, "8 B id + 12 B record, padded to 24");

// Refuse new orders at 75% full: every probe run then ends at an empty entry, and a miss costs
// about 8.5 probes.
constexpr std::uint32_t kMaxLoadNumerator = 3;
constexpr std::uint32_t kMaxLoadDenominator = 4;

// Live orders a table of 2^table_log2 entries may hold (2^4 entries -> 12).
// - The old design's UnorderedMapIndex enforces the same cap, so both refuse the same add and
//   the F0 oracle test stays in lock step.
constexpr std::uint32_t max_live_orders_for(unsigned table_log2) {
  const std::uint32_t entry_count = std::uint32_t{1} << table_log2;
  return entry_count / kMaxLoadDenominator * kMaxLoadNumerator;
}

// order_table_size_log2_for never returns less: 1024 entries. (The constructor itself takes
// down to kMinTableSizeLog2 = 4, which tests use.)
constexpr unsigned kMinAutoSizedTableLog2 = 10;
// At most half full, so probe runs stay short.
constexpr std::uint64_t kTableEntriesPerLiveOrder = 2;

// The table size, as a log2, for a stream that holds about live_orders orders: the smallest
// power of two >= 2 x live_orders, and at least 2^10. The generator's default 16384 live
// orders gives 15 (32768 entries, 768 KB).
inline unsigned order_table_size_log2_for(std::uint64_t live_orders) {
  unsigned table_log2 = kMinAutoSizedTableLog2;
  while ((std::uint64_t{1} << table_log2) < kTableEntriesPerLiveOrder * live_orders) {
    ++table_log2;
  }
  return table_log2;
}

// The order index: a fixed-size open-addressing hash table.
// - The find / is_found / record interface is what lets the old design's UnorderedMapIndex
//   (baseline/book_ladder.hpp) take its place in the F0..F1 benchmark steps.
class OrderTable {
 private:
  std::vector<OrderTableEntry> entries;
  std::uint32_t entry_index_mask = 0;  // entry count - 1
  // 64 - table_log2: keep the top table_log2 bits of the product, the well-mixed ones
  std::uint32_t hash_shift = 0;
  std::uint32_t live_order_cap = 0;
  std::uint32_t live_order_count = 0;

 public:
  // An entry's position. Valid until the next erase: erase shifts later entries back, so a
  // position held across it may name another order. Inserts never move entries.
  using EntryIndex = std::uint32_t;
  static constexpr EntryIndex kNotFound = UINT32_MAX;

  static constexpr unsigned kMinTableSizeLog2 = 4;   // 16 entries, the smallest tests use
  static constexpr unsigned kMaxTableSizeLog2 = 30;  // 2^30 entries x 24 B = 24 GiB

  // 2^64 / golden ratio (1.618...), an odd number. A common choice for multiplicative hashing.
  // - Multiplying mixes each input bit into the bits ABOVE it, so the top bits of the product
  //   are the well-mixed ones. That is why home_entry keeps the top bits (hash_shift).
  // - Odd, so the multiply never loses information.
  // - The payload checksum uses the same number (kChecksumMultiplier, ring_slot.hpp).
  static constexpr std::uint64_t kHashMultiplier = 0x9E3779B97F4A7C15;

  // Startup only. resize() value-initialises every entry, which faults every page in now.
  // - Nothing pins the pages afterwards; the benchmark's zero-page-fault gate would show it if
  //   one were lost.
  explicit OrderTable(unsigned table_log2) {
    check_or_abort(table_log2 >= kMinTableSizeLog2 && table_log2 <= kMaxTableSizeLog2,
                   "OrderTable: 2^4 to 2^30 entries");
    entries.resize(std::size_t{1} << table_log2);
    entry_index_mask = (1u << table_log2) - 1;
    hash_shift = 64 - table_log2;
    live_order_cap = max_live_orders_for(table_log2);
  }

  // Gigabytes at the largest size: never copied by accident.
  OrderTable(const OrderTable&) = delete;
  OrderTable& operator=(const OrderTable&) = delete;

  // The entry holding id, or kNotFound. Walks from id's home entry to the first empty one.
  EntryIndex find(std::uint64_t id) const {
    if (id == kEmptyEntryId) return kNotFound;  // it would match an empty entry
    EntryIndex index = home_entry(id);
    while (entries[index].id != id) {
      if (entries[index].id == kEmptyEntryId) return kNotFound;
      index = (index + 1) & entry_index_mask;
    }
    return index;
  }

  bool is_found(EntryIndex index) const {
    return index != kNotFound;
  }

  OrderRecord& record(EntryIndex index) {
    return entries[index].record;
  }

  // Stores id in the first empty entry from its home on. False when refused: the cap reached,
  // id 0, or a live duplicate.
  // - The cap is what keeps an empty entry in every probe run, so the loop always ends.
  // Example (16 entries; homes: id 9 -> 8, id 17 -> 8, id 14 -> 10, id 22 -> 9):
  //   insert(9)  -> true, entry 8
  //   insert(17) -> true, entry 9   (8 is taken)
  //   insert(14) -> true, entry 10
  //   insert(22) -> true, entry 11  (9 and 10 are taken)
  //   insert(17) -> false           (duplicate);  insert(0) -> false
  bool insert(std::uint64_t id, OrderRecord new_record) {
    if (live_order_count >= live_order_cap || id == kEmptyEntryId) return false;

    EntryIndex index = home_entry(id);
    while (entries[index].id != kEmptyEntryId) {
      if (entries[index].id == id) return false;
      index = (index + 1) & entry_index_mask;
    }

    entries[index] = OrderTableEntry{id, new_record};
    ++live_order_count;
    return true;
  }

  // Deletes the entry at hole, which must be live: a position find returned, with no erase since.
  // - No "deleted" marker: walk the run of entries after the hole. An entry moves back into the
  //   hole only if it is at least as far from its home as from the hole; then a find starting at
  //   its home still passes the hole. An entry whose home lies after the hole stays, or a find
  //   would start past the hole and never reach it.
  // - Each move leaves a new hole further on; the walk ends at the first empty entry.
  // Example (the table after the insert example; erase(8), the entry holding id 9):
  //   entry 9,  id 17, home 8:  distance 1 from home, 1 from hole 8  -> moves to 8, hole 9
  //   entry 10, id 14, home 10: distance 0 from home, 1 from hole 9  -> stays
  //   entry 11, id 22, home 9:  distance 2 from home, 2 from hole 9  -> moves to 9, hole 11
  //   entry 12 is empty: entry 11 is cleared. Now 8: id 17, 9: id 22, 10: id 14.
  void erase(EntryIndex hole) {
    for (EntryIndex candidate = (hole + 1) & entry_index_mask;
         entries[candidate].id != kEmptyEntryId; candidate = (candidate + 1) & entry_index_mask) {
      if (probe_distance(home_entry(entries[candidate].id), candidate) >=
          probe_distance(hole, candidate)) {
        entries[hole] = entries[candidate];
        hole = candidate;
      }
    }
    entries[hole] = OrderTableEntry{};
    --live_order_count;
  }

  // Where a find for id starts.
  EntryIndex home_entry(std::uint64_t id) const {
    return static_cast<EntryIndex>((id * kHashMultiplier) >> hash_shift);
  }

  // Asks for id's home entry ahead of the event that needs it (FeedBook::apply_batch).
  void prefetch(std::uint64_t id) const {
    __builtin_prefetch(&entries[home_entry(id)]);
  }

  std::uint32_t live_orders() const {
    return live_order_count;
  }

  std::uint32_t max_live_orders() const {
    return live_order_cap;
  }

  // - Test-only: every entry is reachable from its home without crossing an empty entry, and
  //   the live count matches the entries in use.
  bool check_invariants() const {
    std::uint32_t entries_in_use = 0;
    for (EntryIndex index = 0; index <= entry_index_mask; ++index) {
      if (entries[index].id == kEmptyEntryId) continue;
      ++entries_in_use;
      for (EntryIndex probe = home_entry(entries[index].id); probe != index;
           probe = (probe + 1) & entry_index_mask) {
        if (entries[probe].id == kEmptyEntryId) return false;
      }
    }
    return entries_in_use == live_order_count;
  }

 private:
  // Entries from `from` forward to `to`, wrapping around the end of the array.
  std::uint32_t probe_distance(EntryIndex from, EntryIndex to) const {
    return (to - from) & entry_index_mask;
  }
};

}  // namespace mdbus::book
