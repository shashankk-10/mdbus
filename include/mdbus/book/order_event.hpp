#pragma once

// The feed handler's input: one order event, one instrument's info, and the price window.
// - An L3 event is order-by-order: it names one order (add, cancel, execute, replace), not a
//   price level.
// - Data flow: exchange -> wire_format.hpp decodes into OrderEvent -> FeedBook::apply.
// - An Add carries both the dense instrument id and the 8-character symbol, as ITCH's add does.
//   The book finds instruments by id; the old design in baseline/ used the symbol.
// - Later events name only the order; the book takes instrument, side and price from its order
//   index.

#include <array>
#include <cstddef>
#include <cstdint>

namespace mdbus::book {

// ITCH symbols: 8 ASCII characters, space padded.
constexpr std::size_t kSymbolLength = 8;
using Symbol = std::array<char, kSymbolLength>;

// Starts at 1, so a zero-filled event matches no case and is ignored.
enum class EventType : std::uint8_t { Add = 1, Cancel = 2, Execute = 3, Replace = 4 };

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

// One L3 event, 48 B.
// - order_id: the order the event is about. For a Replace, the order being replaced.
// - new_order_id: a Replace's new order; 0 for every other type.
// - price: in ticks. Read for Add and Replace only.
// - qty has four meanings: Add and Replace, the order's size; Execute, the quantity filled;
//   Cancel, the quantity removed, where 0 means the whole order.
// - side and symbol: read for Add only.
struct OrderEvent {
  std::uint64_t order_id;
  std::uint64_t new_order_id;
  std::uint64_t exchange_time_ns;
  std::int32_t price;
  std::uint32_t qty;
  std::uint16_t instrument_id;  // dense, 0..N-1
  EventType type;
  Side side;
  Symbol symbol;
  std::uint32_t padding;
};
static_assert(sizeof(OrderEvent) == 48, "fields sum to 48 bytes: no compiler padding");

// One instrument as the simulator and the book both know it.
// - reference_price never moves: the book stores every order price as an offset from it.
struct InstrumentInfo {
  Symbol symbol;
  std::int32_t reference_price;
  std::uint32_t padding;
};

// The price window every book config accepts: an order must rest within -2048..2047 ticks of
// its instrument's reference_price. An add outside it is refused and marks the instrument
// kInstrumentBad.
// - 4096 prices in all = 64 words x 64 bits, the most the ladder's two-level bitmap covers with
//   one 64-bit summary word (price_ladder.hpp).
constexpr std::int32_t kMinPriceOffset = -2048;
constexpr std::int32_t kMaxPriceOffset = 2047;

// The largest 16-bit id, reserved for "not found", so the book takes at most 65534 instruments.
constexpr std::uint16_t kNoInstrument = UINT16_MAX;

// Whether price is better than other_price: higher for a bid, lower for an ask.
constexpr bool is_better_price(bool is_bid, std::int32_t price, std::int32_t other_price) {
  return is_bid ? price > other_price : price < other_price;
}

}  // namespace mdbus::book
