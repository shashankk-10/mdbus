#pragma once

// TopLevels, one side's top levels (feed_book.hpp), and the two rules the writer and the test
// readers share.
// - FeedBook keeps each side's TopLevels with apply_level_change, and publishes what
//   diff_top_levels returns as a BookDelta.
// - Test readers apply those entries with the same apply_level_change (tests/test_helpers.hpp),
//   so their copies must end equal to the writer's.

#include <cstddef>
#include <cstdint>

#include "mdbus/book/order_event.hpp"
#include "mdbus/constants.hpp"
#include "mdbus/messages.hpp"

namespace mdbus::book {

// What find_price_position returns for a price the side does not show.
constexpr std::size_t kNotShown = kTopLevelsPerSide;

// One side's shown levels, best first.
// - Entries past count stay zero, so the array copies straight into an InstrumentSnapshot.
struct TopLevels {
  Level levels[kTopLevelsPerSide]{};
  std::uint8_t count = 0;
};

// Applies one level entry: qty 0 removes the level, any other qty sets it to that absolute
// quantity (inserting it if new), and the side then keeps only its best 6.
// - Recovery does not rely on applying an entry twice: it delivers each delta once, by seq, and
//   absolute quantities make a snapshot plus the deltas after it exact.
// Example (a bid side holding 101x5, 100x3, 99x7):
//   {102, 4} -> 102x4, 101x5, 100x3, 99x7   (insert)
//   {100, 9} -> 101x5, 100x9, 99x7          (update)
//   {100, 0} -> 101x5, 99x7                 (remove)
//   {98, 0}  -> unchanged                   (remove a level not shown: ignored)
// Example (a full bid side 106..101, each x1):
//   {110, 2} -> 110x2, 106x1, ..., 102x1    (insert, 101 pushed out)
//   {100, 2} -> unchanged                   (worse than the 6th: ignored)
inline void apply_level_change(TopLevels& side_levels, bool is_bid, Level level) {
  // 1. Find where the price sits: the first shown level not better than it.
  std::size_t position = 0;
  while (position < side_levels.count &&
         is_better_price(is_bid, side_levels.levels[position].price, level.price)) {
    ++position;
  }
  const bool is_shown = position < side_levels.count &&
                        side_levels.levels[position].price == level.price;

  // 2. A shown level with a new quantity.
  if (is_shown && level.qty != 0) {
    side_levels.levels[position].qty = level.qty;
    return;
  }

  // 3. A shown level is gone: move the worse levels up one place.
  if (is_shown) {
    for (std::size_t j = position; j + 1 < side_levels.count; ++j) {
      side_levels.levels[j] = side_levels.levels[j + 1];
    }
    side_levels.count = static_cast<std::uint8_t>(side_levels.count - 1);
    side_levels.levels[side_levels.count] = Level{0, 0};
    return;
  }

  // 4. Removing a level not shown, or a new level worse than the 6th: no change.
  if (level.qty == 0 || position >= kTopLevelsPerSide) return;

  // 5. A new level: move the worse levels down one place; a full side drops its 6th.
  std::size_t last = side_levels.count;
  if (last == kTopLevelsPerSide) last = kTopLevelsPerSide - 1;
  for (std::size_t j = last; j > position; --j) {
    side_levels.levels[j] = side_levels.levels[j - 1];
  }
  side_levels.levels[position] = level;
  if (side_levels.count < kTopLevelsPerSide) {
    side_levels.count = static_cast<std::uint8_t>(side_levels.count + 1);
  }
}

// The position of price among the shown levels, or kNotShown.
inline std::size_t find_price_position(const TopLevels& side_levels, std::int32_t price) {
  for (std::size_t i = 0; i < side_levels.count; ++i) {
    if (side_levels.levels[i].price == price) return i;
  }
  return kNotShown;
}

// Writes the entries that turn `before` into `after` under apply_level_change, and returns how
// many. delta_entries needs room for 2 x 6 (every level removed, 6 new ones).
// - Removals go first, so the reader's copy shrinks to the levels both share before the new
//   ones arrive, and the cut to 6 never drops a level `after` needs.
// - One event gives at most BookDelta::kMaxEntries = 3 entries, and the bound is reached. Both
//   3-entry cases are a replace: its old level empties, a 7th level is pulled up and its new
//   (shown) level changes; or its old level shrinks, its new level is inserted and the 6th is
//   pushed out. A refill and a push-out in one event cancel, leaving 2.
// Example (a full bid side 106..101 x1, 100x1 behind it; a replace moves order 104x1 to 102):
//   -> 3 entries: {104, 0}, {102, 2}, {100, 1}   (the removal first, then best first)
inline std::size_t diff_top_levels(const TopLevels& before, const TopLevels& after,
                                   Level* delta_entries) {
  std::size_t count = 0;

  // 1. Removals: the levels `after` no longer shows, best first.
  for (std::size_t i = 0; i < before.count; ++i) {
    if (find_price_position(after, before.levels[i].price) == kNotShown) {
      delta_entries[count] = Level{before.levels[i].price, 0};
      ++count;
    }
  }

  // 2. New levels and levels whose quantity changed, best first.
  for (std::size_t j = 0; j < after.count; ++j) {
    const std::size_t before_position = find_price_position(before, after.levels[j].price);
    if (before_position == kNotShown || before.levels[before_position].qty != after.levels[j].qty) {
      delta_entries[count] = after.levels[j];
      ++count;
    }
  }
  return count;
}

}  // namespace mdbus::book
