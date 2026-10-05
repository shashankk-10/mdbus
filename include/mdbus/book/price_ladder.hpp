#pragma once

// FeedBook's level policy: the total qty at every price of the window, per side per instrument.
// - "Price ladder" here is the standard trading term (a qty at every price). The F0..F4 "book
//   ladder" of benchmark steps is baseline/book_ladder.hpp.
// - A level lookup is a subtract and an array index; no tree, no hash.
// - A two-level bitmap of the non-empty levels finds the next level when the best one empties:
//   two bit scans instead of a walk over empty prices.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "mdbus/book/order_event.hpp"
#include "mdbus/constants.hpp"

namespace mdbus::book {

// An offset no real level has (real ones are -2048..2047).
constexpr std::int32_t kNoLevel = INT32_MIN;

// One side of one instrument.
// - Index i holds offset kMinPriceOffset + i, so index 0 is offset -2048.
// - nonempty_level_bits: bit i % 64 of word i / 64 is set when level i is non-empty.
// - nonempty_word_bits: bit w is set when nonempty_level_bits[w] is non-zero.
// - Aligned to a 64 B line, so best_index and nonempty_word_bits, the two words most changes
//   read, share one line.
// Example (the bits for a level at offset 25):
//   index 25 + 2048 = 2073 -> nonempty_level_bits[32] bit 25, and nonempty_word_bits bit 32
template <bool kIsBid>
class alignas(kL1LineBytes) PriceLadderSide {
 public:
  static constexpr std::int32_t kWindowSize = kMaxPriceOffset - kMinPriceOffset + 1;  // 4096
  static constexpr std::int32_t kBitsPerWord = 64;
  static constexpr std::int32_t kLevelBitWordCount = kWindowSize / kBitsPerWord;  // 64
  static_assert(kWindowSize % kBitsPerWord == 0 && kLevelBitWordCount <= kBitsPerWord,
                "whole level-bit words, and one summary word covers them all");
  static constexpr std::int32_t kNoIndex = -1;  // "no such level", as an index

 private:
  std::int32_t best_index = kNoIndex;  // kNoIndex when the side is empty
  std::uint64_t nonempty_word_bits = 0;
  std::uint64_t nonempty_level_bits[kLevelBitWordCount]{};
  std::uint32_t qty_at_index[kWindowSize]{};

 public:
  // The best level's offset, or kNoLevel when the side is empty.
  std::int32_t best_offset() const {
    if (best_index < 0) return kNoLevel;
    return kMinPriceOffset + best_index;
  }

  // The best level strictly worse than price_offset, or kNoLevel; qty is set only when one is
  // found.
  // - price_offset must lie in the window (FeedBook passes a level it shows); it need not be a
  //   level itself.
  // Example (levels at offsets 25 x100 and -3 x50):
  //   bid: next_worse_level(25) -> -3, qty 50;  next_worse_level(-3) -> kNoLevel
  //   ask: next_worse_level(-3) -> 25, qty 100
  std::int32_t next_worse_level(std::int32_t price_offset, std::uint32_t& qty) const {
    const std::int32_t index = price_offset - kMinPriceOffset;
    std::int32_t found_index = kNoIndex;
    if constexpr (kIsBid) {
      found_index = highest_at_or_below(std::min(index - 1, kWindowSize - 1));
    } else {
      found_index = lowest_at_or_above(std::max(index + 1, 0));
    }
    if (found_index < 0) return kNoLevel;
    qty = qty_at_index[found_index];
    return kMinPriceOffset + found_index;
  }

  // Adds qty > 0 at price_offset and returns the level's new total, or 0 when price_offset is
  // outside the window.
  // - Totals are 32-bit and not checked for overflow.
  // - add_qty checks the window and remove_qty does not: a price enters the ladder only here,
  //   so a remove is always for a price that passed this check. (FeedBook also checks the
  //   window before it stores an order; the ladder test relies on this one.)
  std::uint32_t add_qty(std::int32_t price_offset, std::uint32_t qty) {
    const std::int32_t index = price_offset - kMinPriceOffset;
    if (!is_inside_window(index)) return 0;
    qty_at_index[index] += qty;
    if (qty_at_index[index] == qty) {  // the level was empty
      set_bit(index);
      if (best_index < 0 || is_better_price(kIsBid, index, best_index)) best_index = index;
    }
    return qty_at_index[index];
  }

  // Removes qty from the level at price_offset and returns what is left.
  // - The level must hold at least qty, and price_offset must lie in the window; neither is
  //   checked. FeedBook never removes more than an order holds.
  std::uint32_t remove_qty(std::int32_t price_offset, std::uint32_t qty) {
    const std::int32_t index = price_offset - kMinPriceOffset;
    qty_at_index[index] -= qty;
    const std::uint32_t qty_left = qty_at_index[index];
    if (qty_left == 0) {
      clear_bit(index);
      // Nothing was better than the best, so the new best is the next level worse than it:
      // usually in the same nonempty_level_bits word, without reading nonempty_word_bits.
      if (index == best_index) {
        if constexpr (kIsBid) {
          best_index = highest_at_or_below(index - 1);
        } else {
          best_index = lowest_at_or_above(index + 1);
        }
      }
    }
    return qty_left;
  }

 private:
  static bool is_inside_window(std::int32_t index) {
    return index >= 0 && index < kWindowSize;
  }

  // Bits 0..bit-1 of a word, and bits 0..bit, for bit in 0..63.
  static std::uint64_t bits_below(std::int32_t bit) {
    return (std::uint64_t{1} << bit) - 1;
  }

  static std::uint64_t bits_up_to(std::int32_t bit) {
    return ~std::uint64_t{0} >> (kBitsPerWord - 1 - bit);
  }

  // The highest and lowest set bit of a non-zero word.
  static std::int32_t highest_bit(std::uint64_t word_bits) {
    return kBitsPerWord - 1 - __builtin_clzll(word_bits);
  }

  static std::int32_t lowest_bit(std::uint64_t word_bits) {
    return __builtin_ctzll(word_bits);
  }

  void set_bit(std::int32_t index) {
    const std::int32_t word = index / kBitsPerWord;
    const std::int32_t bit = index % kBitsPerWord;
    nonempty_level_bits[word] |= std::uint64_t{1} << bit;
    nonempty_word_bits |= std::uint64_t{1} << word;
  }

  void clear_bit(std::int32_t index) {
    const std::int32_t word = index / kBitsPerWord;
    const std::int32_t bit = index % kBitsPerWord;
    nonempty_level_bits[word] &= ~(std::uint64_t{1} << bit);
    if (nonempty_level_bits[word] == 0) nonempty_word_bits &= ~(std::uint64_t{1} << word);
  }

  // The highest non-empty index <= index, or kNoIndex. index is in -1..kWindowSize-1.
  // 1. Look in index's own word, at or below its bit.
  // 2. Otherwise the summary word names the nearest lower non-empty word; take its highest bit.
  std::int32_t highest_at_or_below(std::int32_t index) const {
    if (index < 0) return kNoIndex;
    const std::int32_t word = index / kBitsPerWord;
    const std::int32_t bit = index % kBitsPerWord;
    const std::uint64_t in_word = nonempty_level_bits[word] & bits_up_to(bit);
    if (in_word != 0) return kBitsPerWord * word + highest_bit(in_word);

    const std::uint64_t lower_words = nonempty_word_bits & bits_below(word);
    if (lower_words == 0) return kNoIndex;
    const std::int32_t found_word = highest_bit(lower_words);
    return kBitsPerWord * found_word + highest_bit(nonempty_level_bits[found_word]);
  }

  // The lowest non-empty index >= index, or kNoIndex. index is in 0..kWindowSize. The mirror of
  // highest_at_or_below.
  std::int32_t lowest_at_or_above(std::int32_t index) const {
    if (index >= kWindowSize) return kNoIndex;
    const std::int32_t word = index / kBitsPerWord;
    const std::int32_t bit = index % kBitsPerWord;
    const std::uint64_t in_word = nonempty_level_bits[word] & ~bits_below(bit);
    if (in_word != 0) return kBitsPerWord * word + lowest_bit(in_word);

    const std::uint64_t higher_words = nonempty_word_bits & ~bits_up_to(word);
    if (higher_words == 0) return kNoIndex;
    const std::int32_t found_word = lowest_bit(higher_words);
    return kBitsPerWord * found_word + lowest_bit(nonempty_level_bits[found_word]);
  }
};

// A PriceLadderSide per side per instrument, about 33 KB per instrument.
// - next_worse_level is what lets FeedBook refill a full side with one bitmap walk when one of
//   its 6 shown levels empties.
// - side is a Side value: 0 bid, 1 ask.
class PriceLadder {
 private:
  struct BidAndAskSides {
    PriceLadderSide<true> bid;
    PriceLadderSide<false> ask;
  };

  std::vector<BidAndAskSides> sides_by_instrument;

 public:
  // Value-initialised, so every page is touched now, before the first event.
  explicit PriceLadder(std::uint32_t instrument_count) : sides_by_instrument(instrument_count) {}

  std::uint32_t add_qty(std::uint16_t instrument_id, std::uint8_t side, std::int32_t price_offset,
                        std::uint32_t qty) {
    if (side == 0) return sides_by_instrument[instrument_id].bid.add_qty(price_offset, qty);
    return sides_by_instrument[instrument_id].ask.add_qty(price_offset, qty);
  }

  std::uint32_t remove_qty(std::uint16_t instrument_id, std::uint8_t side,
                           std::int32_t price_offset, std::uint32_t qty) {
    if (side == 0) return sides_by_instrument[instrument_id].bid.remove_qty(price_offset, qty);
    return sides_by_instrument[instrument_id].ask.remove_qty(price_offset, qty);
  }

  std::int32_t next_worse_level(std::uint16_t instrument_id, std::uint8_t side,
                                std::int32_t price_offset, std::uint32_t& qty) const {
    if (side == 0) {
      return sides_by_instrument[instrument_id].bid.next_worse_level(price_offset, qty);
    }
    return sides_by_instrument[instrument_id].ask.next_worse_level(price_offset, qty);
  }
};

}  // namespace mdbus::book
