// The price ladder and the order table against brute-force models.
// - Ladder: a side whose mid sweeps and jumps across the whole price window, and past both
//   edges, must refuse every add outside the window and agree with a std::map. It is checked on
//   its best level after every step and walked level by level at intervals.
// - Order table: churns near its load cap over a small id space (duplicates, long probe
//   clusters), must agree with an unordered_map, refuse inserts past 3/4 load, and keep every
//   entry reachable after deletes.
// - Ladder at the ends of int32: every offset far outside the window is refused (and, under
//   UBSan, without overflow).
// - A failure means the book can lose or misplace a level or an order.

#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mdbus/book/order_table.hpp"
#include "mdbus/book/price_ladder.hpp"
#include "test_harness.hpp"

using namespace mdbus::book;

namespace {

// A uniform draw below n from a seeded mt19937_64.
struct TestRandom {
  std::mt19937_64 engine;

  explicit TestRandom(std::uint64_t seed) : engine(seed) {}

  std::uint64_t below(std::uint64_t n) {
    return engine() % n;
  }
};

// The mid sweeps one tick every kStepsPerSweepTick steps from 100 ticks past one edge of the
// window to 100 past the other and back, so adds land outside the window on both sides. On top of
// the sweep it moves -1, 0 or +1 every step, and about every 512 steps jumps by up to 200 ticks.
constexpr int kStepsPerSweepTick = 16;
constexpr std::int32_t kMidPriceSlack = 100;
constexpr std::int32_t kStepsPerMidJump = 512;
constexpr std::int32_t kMaxMidPriceJump = 200;

template <bool kIsBid>
void ladder_vs_model(std::uint64_t seed) {
  // One tick past the window's best edge: next_worse_level from here finds the best level.
  constexpr std::int32_t kPastBestEdge = kIsBid ? kMaxPriceOffset + 1 : kMinPriceOffset - 1;
  PriceLadderSide<kIsBid> ladder;
  std::map<std::int32_t, std::uint32_t> model;
  TestRandom r(seed);
  std::int32_t mid = 0;
  std::int32_t direction = 1;
  std::uint64_t bad = 0;
  std::uint64_t refused_low = 0;  // adds below the window, and above it
  std::uint64_t refused_high = 0;

  for (int step = 0; step < 200'000; ++step) {
    if (step % kStepsPerSweepTick == 0) mid += direction;
    if (mid >= kMaxPriceOffset + kMidPriceSlack) direction = -1;
    if (mid <= kMinPriceOffset - kMidPriceSlack) direction = 1;
    if (r.below(kStepsPerMidJump) == 0) {
      mid += static_cast<std::int32_t>(r.below(2 * kMaxMidPriceJump + 1)) - kMaxMidPriceJump;
    } else {
      mid += static_cast<std::int32_t>(r.below(3)) - 1;
    }
    mid =
        std::max(kMinPriceOffset - kMidPriceSlack, std::min(mid, kMaxPriceOffset + kMidPriceSlack));

    if (model.empty() || r.below(100) < 55) {
      // Depth behind the mid: mostly near it, now and then 300 ticks deeper.
      std::int32_t depth = static_cast<std::int32_t>(r.below(16) + r.below(48));
      if (r.below(100) == 0) depth += 300;
      const std::int32_t price = kIsBid ? mid - depth : mid + depth;
      const auto q = static_cast<std::uint32_t>(1 + r.below(100));
      const std::uint32_t total = ladder.add_qty(price, q);
      if (price < kMinPriceOffset || price > kMaxPriceOffset) {
        if (price < kMinPriceOffset) ++refused_low;
        else ++refused_high;
        if (total != 0) ++bad;  // a refusal leaves the model alone
      } else {
        model[price] += q;
        if (total != model[price]) ++bad;
      }
    } else {
      const auto it = std::next(model.begin(), static_cast<long>(r.below(model.size())));
      const auto q = static_cast<std::uint32_t>(1 + r.below(it->second));
      it->second -= q;
      if (ladder.remove_qty(it->first, q) != it->second) ++bad;
      if (it->second == 0) model.erase(it);
    }

    std::int32_t want_best = kNoLevel;
    if (!model.empty()) want_best = kIsBid ? model.rbegin()->first : model.begin()->first;
    std::uint32_t q = 0;
    if (ladder.next_worse_level(kPastBestEdge, q) != want_best) ++bad;
    if (step % 256 != 0 || model.empty()) continue;

    // The whole side, walked by next_worse_level from past the best edge, equals the model:
    // every level and quantity, in order. The model's levels best first: the highest price first
    // for bids, the lowest for asks.
    std::vector<std::pair<std::int32_t, std::uint32_t>> expected(model.begin(), model.end());
    if (kIsBid) std::reverse(expected.begin(), expected.end());
    std::int32_t price = kPastBestEdge;
    for (const std::pair<std::int32_t, std::uint32_t>& level : expected) {
      price = ladder.next_worse_level(price, q);
      if (price != level.first || q != level.second) ++bad;
    }
    if (ladder.next_worse_level(price, q) != kNoLevel) ++bad;
  }

  std::cerr << "  " << (kIsBid ? "bid" : "ask") << ": " << refused_low << " adds below the window, "
            << refused_high << " above it\n";
  CHECK(bad == 0 && refused_low > 0 && refused_high > 0);
}

}  // namespace

// The ladder walk above, against std::map.
TEST(ladder_matches_model) {
  ladder_vs_model<true>(0x1234567);
  ladder_vs_model<false>(0x7654321);
}

// The order table churn above, against std::unordered_map.
TEST(order_table_matches_model) {
  for (unsigned table_log2 : {6u, 12u}) {
    OrderTable table(table_log2);
    std::unordered_map<std::uint64_t, std::uint32_t> model;
    TestRandom r(table_log2 * 977u);
    std::uint64_t bad = 0;
    std::uint64_t full = 0;
    std::uint64_t dups = 0;
    const std::uint64_t entry_count = std::uint64_t{1} << table_log2;

    for (int step = 0; step < 300'000; ++step) {
      const std::uint64_t id =
          1 + r.below(entry_count + entry_count / 2);  // ids repeat: duplicates
      if (r.below(2) == 0) {
        OrderRecord record{};
        record.remaining_qty = static_cast<std::uint32_t>(1 + r.below(1000));
        const bool ok = table.insert(id, record);
        const bool at_cap = model.size() >= max_live_orders_for(table_log2);
        const bool is_dup = model.count(id) != 0;
        if (ok != (!at_cap && !is_dup)) ++bad;
        if (!ok && at_cap) ++full;
        if (!ok && !at_cap) ++dups;
        if (ok) model[id] = record.remaining_qty;
      } else {
        const OrderTable::EntryIndex entry = table.find(id);
        const auto it = model.find(id);
        if ((entry != OrderTable::kNotFound) != (it != model.end())) ++bad;
        if (entry == OrderTable::kNotFound || it == model.end()) continue;
        if (table.record(entry).remaining_qty != it->second) ++bad;
        table.erase(entry);
        model.erase(it);
      }

      if (step % 997 == 0 && (!table.check_invariants() || table.live_orders() != model.size()))
        ++bad;
    }

    std::cerr << "  2^" << table_log2 << " entries: " << full << " refused at the cap, " << dups
              << " duplicates\n";
    CHECK(bad == 0 && full > 0 && dups > 0 && table.check_invariants());
  }
}

// add_qty's own window check, used standalone at the ends of int32: every offset outside the
// window is refused with 0 and leaves the side empty. The target traps on signed overflow, so
// this also shows the check does not overflow (offset - kMinPriceOffset once did, from
// INT32_MAX - 2047 up).
TEST(ladder_refuses_offsets_far_outside_the_window) {
  PriceLadderSide<true> bids;
  PriceLadderSide<false> asks;
  for (const std::int32_t offset :
       {INT32_MIN, kMinPriceOffset - 1, kMaxPriceOffset + 1, INT32_MAX - 2047, INT32_MAX}) {
    CHECK(bids.add_qty(offset, 1) == 0);
    CHECK(asks.add_qty(offset, 1) == 0);
  }
  std::uint32_t q = 0;
  CHECK(bids.next_worse_level(kMaxPriceOffset + 1, q) == kNoLevel);
  CHECK(asks.next_worse_level(kMinPriceOffset - 1, q) == kNoLevel);

  // Both edges are inside.
  CHECK(bids.add_qty(kMinPriceOffset, 5) == 5 && bids.add_qty(kMaxPriceOffset, 7) == 7);
  CHECK(bids.next_worse_level(kMaxPriceOffset, q) == kMinPriceOffset && q == 5);
}
